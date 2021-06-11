package Plugins::UPnPBridge::Plugin;

use strict;

use Data::Dumper;
use File::Spec::Functions;
use XML::Simple;
use base qw(Slim::Plugin::Base);

use Slim::Utils::Prefs;
use Slim::Utils::Log;
use Slim::Control::Request;

my $prefs = preferences('plugin.upnpbridge');
my $hasOutputChannels;
my $statusHandler = Slim::Control::Request::addDispatch(['status', '_index', '_quantity'], [1, 1, 1, \&statusQuery]);

$prefs->init({ 
	autorun => 1, opts => '', 
	debugs => '', 
	logging => 0, 
	bin => undef, 
	configfile => "upnpbridge.xml", 
	profilesURL => initProfilesURL(), 
	autosave => 1, 
	eraselog => 0,
	baseport => '',
});

my $log = Slim::Utils::Log->addLogCategory({
	'category'     => 'plugin.upnpbridge',
	'defaultLevel' => 'WARN',
	'description'  => Slim::Utils::Strings::string('PLUGIN_UPNPBRIDGE'),
}); 

sub hasOutputChannels {
	my ($self) = @_;
	return $hasOutputChannels->(@_) unless $self->modelName =~ /UPnPBridge/;
	return 0;
}

sub initPlugin {
	my $class = shift;

	# this is hacky but I won't redefine a whole player model just for this	
	require Slim::Player::SqueezePlay;
	$hasOutputChannels = Slim::Player::SqueezePlay->can('hasOutputChannels');
	*Slim::Player::SqueezePlay::hasOutputChannels = \&hasOutputChannels;

	$class->SUPER::initPlugin(@_);
	
	require Plugins::UPnPBridge::Squeeze2upnp;		
	
	if ($prefs->get('autorun')) {
		Plugins::UPnPBridge::Squeeze2upnp->start;
	}
	
	if (!$::noweb) {
		require Plugins::UPnPBridge::Settings;
		Plugins::UPnPBridge::Settings->new;
		Slim::Web::Pages->addPageFunction("^upnpbridge-log.log", \&Plugins::UPnPBridge::Squeeze2upnp::logHandler);
		Slim::Web::Pages->addPageFunction("^upnpbridge-config.xml", \&Plugins::UPnPBridge::Squeeze2upnp::configHandler);
		Slim::Web::Pages->addPageFunction("upnpbridge-guide.htm", \&Plugins::UPnPBridge::Squeeze2upnp::guideHandler);
	}
	
	$log->warn(Dumper(Slim::Utils::OSDetect::details()));
}

sub initProfilesURL {
	my $file = catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'install.xml');
	return XMLin($file, ForceArray => 0, KeepRoot => 0, NoAttr => 0)->{'profilesURL'};
}

sub shutdownPlugin {
	if ($prefs->get('autorun')) {
		Plugins::UPnPBridge::Squeeze2upnp->stop;
	}
}

sub statusQuery {
	my ($request) = @_;
	my $song = $request->client->playingSong if $request->client;

	$statusHandler->($request);

	my $song = $request->client->playingSong if $request->client;
	return unless $song;

	my $handler = $song->currentTrackHandler;
	return unless $handler;

	$request->addResult("repeating_stream", 1) if $handler->can('isRepeatingStream') && $handler->isRepeatingStream($song);
}

1;
