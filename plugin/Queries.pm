package Plugins::UPnPBridge::Queries;

use strict;

use Slim::Utils::Log;
use Slim::Web::ImageProxy qw(proxiedImage);

my $log = logger('plugin.upnpbridge');
my $statusHandler;

sub initQueries {
	Slim::Control::Request::addDispatch(['repeatingsonginfo'], [1, 1, 1, \&repeatingSonginfoQuery]);
	$statusHandler = Slim::Control::Request::addDispatch(['status', '_index', '_quantity'], [1, 1, 1, \&statusQuery]);
}	

sub statusQuery {
	my ($request) = @_;
	my $song = $request->client->playingSong;
	
	$statusHandler->($request);
	return if !$song;
	
	my $handler = $song->currentTrackHandler;
	$request->addResult("repeating_stream", 1) if $handler && $handler->can('isRepeatingStream') && $handler->isRepeatingStream($song);
}

sub repeatingSonginfoQuery {
	my $request = shift;
	my $handler = $request->client->streamingSong->currentTrackHandler;

	# check this is the correct query.
	if ($request->isNotQuery([['repeatingsonginfo']]) || !$handler || !$handler->can('isRepeatingStream')) {
		$request->setStatusBadDispatch();
		return;
	}

	my $tags  = 'abcdefghijJklmnopqrstvwxyzBCDEFHIJKLMNOQRTUVWXY'; # all letter EXCEPT u, A & S, G & P, Z
	my $tagsprm  = $request->getParam('tags');

	# did we have override on the defaults?
	$tags = $tagsprm if defined $tagsprm;

	my $hashRef = _songData($request, $tags);
	my $loopname = 'streamingsonginfo_loop';
	my $chunkCount = 0;

	# this is where we construct the nowplaying menu
	while (my ($key, $val) = each %{$hashRef}) {
		$request->addResultLoop($loopname, $chunkCount, $key, $val);
		$chunkCount++;
	}
	
	$request->setStatusDone();
}

my %tagMap = (
	# Tag    Tag name             Token            Track method         Track field
	#------------------------------------------------------------------------------
	  'u' => ['url',              'LOCATION',      'url'],              #url
	  'o' => ['type',             'TYPE',          'content_type'],     #content_type
	  'a' => ['artist',           'ARTIST',        'artistName'],       #->contributors
	  'l' => ['album',            'ALBUM',         'albumname'],        #->album.title
	  't' => ['tracknum',         'TRACK',         'tracknum'],         #tracknum
	  'i' => ['disc',             'DISC',          'disc'],             #disc
	  'j' => ['coverart',         'SHOW_ARTWORK',  'coverArtExists'],   #cover
	  'd' => ['duration',         'LENGTH',        'secs'],             #secs
	  'r' => ['bitrate',          'BITRATE',       'prettyBitRate'],    #bitrate
	  'T' => ['samplerate',       'SAMPLERATE',    'samplerate'],       #samplerate
	  'I' => ['samplesize',       'SAMPLESIZE',    'samplesize'],       #samplesize
	  'H' => ['channels',         'CHANNELS',      'channels'],         #channels
	  'c' => ['coverid',          'COVERID',       'coverid'],          # coverid
	  'K' => ['artwork_url',      '',              'coverurl'],         # artwork URL, not in db
	  'B' => ['buttons',          '',              'buttons'],          # radio stream special buttons	  
	  'L' => ['info_link',        '',              'info_link'],        # special trackinfo link for i.e. Pandora
	  'N' => ['remote_title'],                                          # remote stream title
);

sub _songData {
	my ($request, $tags) = @_;
	my $song	= $request->client->streamingSong;
	my $track = $song->track;

	# If we have a remote track, check if a plugin can provide metadata
	my $remoteMeta = {};
		
	my $handler = $song->currentTrackHandler;

	if ( $handler && $handler->can('getMetadataFor') ) {
		# Don't modify source data
		$remoteMeta = Storable::dclone(
			$handler->getMetadataFor( $request->client, $track->url, 'repeating' )
		);

		$remoteMeta->{a} = $remoteMeta->{artist};
		$remoteMeta->{A} = $remoteMeta->{artist};
		$remoteMeta->{l} = $remoteMeta->{album};
		$remoteMeta->{i} = $remoteMeta->{disc};
		$remoteMeta->{K} = $remoteMeta->{cover};
		$remoteMeta->{d} = ( $remoteMeta->{duration} || 0 ) + 0;
		$remoteMeta->{Y} = $remoteMeta->{replay_gain};
		$remoteMeta->{o} = $remoteMeta->{type};
		$remoteMeta->{r} = $remoteMeta->{bitrate};
		$remoteMeta->{B} = $remoteMeta->{buttons};
		$remoteMeta->{L} = $remoteMeta->{info_link};
		$remoteMeta->{t} = $remoteMeta->{tracknum};
	}

	my %returnHash;

	# define an ordered hash for our results
	tie (%returnHash, "Tie::IxHash");

	$returnHash{'id'}    = $track->id;
	$returnHash{'title'} = $remoteMeta->{title} || $track->title;

	# loop so that stuff is returned in the order given...
	for my $tag (split (//, $tags)) {

		my $tagref = $tagMap{$tag} or next;

		# special case, remote stream name
		if ($tag eq 'N') {
			if (!$track->secs && $remoteMeta->{title} && !$remoteMeta->{album} ) {
				if (my $meta = $track->title) {
					$returnHash{$tagref->[0]} = $meta;
				}
			}
		}

		elsif ($tag eq 'A') {
			if ( my $meta = $remoteMeta->{$tag} ) {
				$returnHash{artist} = $meta;
			}
		}	

		# if we have a method/relationship for the tag
		elsif (defined(my $method = $tagref->[2])) {

			my $value;
			my $key = $tagref->[0];

			# Override with remote track metadata if available
			if ( defined $remoteMeta->{$tag} ) {
				$value = $remoteMeta->{$tag};
			}

			elsif ($method eq '' || !$track->can($method)) {
				next;
			}

			# simple track method
			else {
				$value = $track->$method();
			}

			# we might need to proxy the image request to resize it
			if ($tag eq 'K' && $value) {
				$value = proxiedImage($value);
			}

			# if we have a value
			if (defined $value && $value ne '') {
				# add the tag to the result
				$returnHash{$key} = $value;
			}
		}
	}

	return \%returnHash;
}


1;
