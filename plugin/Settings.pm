package Plugins::UPnPBridge::Settings;

use strict;

use File::Spec::Functions;
use LWP::Simple;
use base qw(Slim::Web::Settings);
use XML::Simple;
use Data::Dump qw(dump);
use Slim::Utils::PluginManager;
use Slim::Utils::Prefs;
use Slim::Utils::Log;

my $prefs = preferences('plugin.upnpbridge');
my $log   = logger('plugin.upnpbridge');
my @xmlmain = qw(binding log_limit custom_discovery);
my @xmldevice = qw(name mac stream_length accept_nexturi next_delay sample_rate mode codecs L24_format flac_header enabled remove_timeout send_metadata volume_on_play max_volume raw_audio_format send_coverart coverart seek_after_pause live_pause send_icy volume_feedback server forced_mimetypes);
my @prefs_bool  = qw(autorun logging autosave eraselog useLMSsocket);
my @prefs_other = qw(output bin debugs opts baseport);

sub name { 'PLUGIN_UPNPBRIDGE' }

sub page { 'plugins/UPnPBridge/settings/basic.html' }
	
sub handler {
	my ($class, $client, $params, $callback, @args) = @_;
	my $process;
	
	require Plugins::UPnPBridge::Squeeze2upnp;
	require Plugins::UPnPBridge::Plugin;
			
	if ($params->{'updateprofiles'}) {			
		$log->debug("update profile required: ", $params->{'updatefrom'});
		
		$prefs->set('profilesURL', $params->{'updatefrom'});		
		my $content = get($prefs->get('profilesURL'));
		if ($content) {
			open(my $fh, ">", catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'profiles.xml')); 
			print $fh $content;
			close $fh;
		}
	} elsif ($params->{ 'delconfig' }) {
		my $conf = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
		unlink $conf;							
		$log->info("deleting configuration $conf");
	} elsif ($params->{ 'genconfig' }) {
		$log->info("generating configuration ", Plugins::UPnPBridge::Squeeze2upnp->configFile($class));
		$process = { cb => \&genConfig };		
	} elsif ($params->{ 'cleanlog' }) {
		my $logfile = Plugins::UPnPBridge::Squeeze2upnp->logFile($class);
		open my $fh, ">", $logfile;
		print $fh;
		close $fh;
	} elsif ($params->{ 'restart' }) {
		Plugins::UPnPBridge::Squeeze2upnp->stop;
		waitEndHandler(undef, $class, $client, $params, $callback, 30, @args);
	} elsif ($params->{'saveSettings'}) {
		my $update;

		$log->debug("save settings required");

		# transcode special parameters
		$params->{mode} = $params->{encode_mode};
		if ( $params->{encode_mode} ) {
			if ($params->{encode_mode} eq 'flc') {
				$params->{mode} .=  ":$params->{encode_level}" if defined $params->{encode_level} && $params->{encode_level} ne '';
			} elsif ($params->{encode_mode} eq 'mp3') {
				$params->{mode} .=  ":$params->{encode_bitrate}" if $params->{encode_bitrate};
			} 
			if ($params->{encode_mode} && $params->{encode_mode} ne 'thru') {
				$params->{mode} .= ",r:" . ($params->{encode_rate_flag} ? "-" : "") . $params->{encode_rate} if $params->{encode_rate};
				$params->{mode} .= ",s:$params->{encode_size}" if $params->{encode_size};
				$params->{mode} .= ",flow" if $params->{encode_flow};
			}	
		}	
						
		for my $param (@prefs_bool) {
			my $val = $params->{ $param } ? 1 : 0;
			
			if ($val != $prefs->get($param)) {
				$prefs->set($param, $val);
				$update = 1;
			}
		}
		
		# check that the config file name has not changed first
		for my $param (@prefs_other) {
			if ($params->{ $param } ne $prefs->get($param)) {
				$prefs->set($param, $params->{ $param });
				$update = 1;
			}
		}
		
		my $xmlconfig = readconfig($class, KeyAttr => 'device');
		
		if ($params->{ 'configfile' } ne $prefs->get('configfile')) {
			$prefs->set('configfile', $params->{ 'configfile' });
			if (-e Plugins::UPnPBridge::Squeeze2upnp->configFile($class)) {
				$update = 0;
				undef $xmlconfig;
			} 
		}					
		
		# get XML player configuration if current device has changed in the list
		if ($xmlconfig && ($params->{'seldevice'} eq $params->{'prevseldevice'})) {
		
			for my $p (@xmlmain) {
				next if !defined $params->{ $p };
				
				if ($params->{ $p } eq '') {
					delete $xmlconfig->{ $p };
				} else {
					$xmlconfig->{ $p } = $params->{ $p };
				}	
			}
			
			$log->info("current: ", $params->{'seldevice'}, "previous: ", $params->{'prevseldevice'});
			
			#save common parameters
			if ($params->{'seldevice'} eq '.common.') {
				for my $p (@xmldevice) {
					next if !defined $params->{ $p };
					if ($params->{ $p } eq '') {
						delete $xmlconfig->{ common }->{ $p };
					} else {
						$xmlconfig->{ common }->{ $p } = $params->{ $p };
					}
				}	
				
			} else {
				if ($params->{'deldevice'}) {
					#delete current device	
					$log->info(@{$xmlconfig->{'device'}});
					@{$xmlconfig->{'device'}} = grep $_->{'udn'} ne $params->{'seldevice'}, @{$xmlconfig->{'device'}};
					$params->{'seldevice'} = '.common.';
					
				} else {
					# save player specific parameters
					$params->{'devices'} = \@{$xmlconfig->{'device'}};
					my $device = findUDN($params->{'seldevice'}, $params->{'devices'});
					
					for my $p (@xmldevice) {
						next if !defined $params->{ $p };
						if ($params->{ $p } eq '') {
							delete $device->{ $p };
						} else {
							$device->{ $p } = $params->{ $p };
						}
					}	
				
					# a profile has been applied, then overwrite some of the above
					if ($params->{ 'applyprofile' }) {
						my $profile = loadprofiles()->{ $params->{'selprofile'} };
						
						foreach (keys %$profile) {
							$device->{ $_ } = $profile-> { $_ };
						}	
					}	
				}			
			}	
			
			# get enabled status for all device, except the selected one (if any)
			foreach my $device (@{$xmlconfig->{'device'}}) {
				if ($device->{'udn'} ne $params->{'seldevice'}) {
					my $enabled = $params->{ 'enabled.'.$device->{ 'udn' } };
					$device->{'enabled'} = defined $enabled ? $enabled : 0;
				}	
			}
			
			$log->info("writing XML config");
			$log->debug(dump($xmlconfig));
			
			$update = 1;
		}	
		
		if ($update) {
			my $writeXML = sub {
				my $conf = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
				
				return if !$xmlconfig;
				$log->debug("write file now");
				XMLout(	$xmlconfig, RootName => "squeeze2upnp", NoSort => 1, NoAttr => 1, OutputFile => $conf );
			};
			
			$process = { cb => $writeXML, handler => 1 };
		}	
	}

	# something has been updated, XML array is up-to-date anyway, but need to write it
	if ($process) {
		$log->debug("full processing");
		Plugins::UPnPBridge::Squeeze2upnp->stop;
		waitEndHandler($process, $class, $client, $params, $callback, 30, @args);
	} else {
		# just re-read config file and update page
		$log->debug("not updating");
		$class->handler2($client, $params, $callback, @args);		  
	}

	return undef;
}

sub waitEndHandler	{
	my ($process, $class, $client, $params, $callback, $wait, @args) = @_;
	my $page;
	
	if ( Plugins::UPnPBridge::Squeeze2upnp->alive() ) {
		$log->debug('Waiting for squeeze2upnp to end');
		$wait--;
		if ($wait) {
			Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 1, sub {
				waitEndHandler($process, $class, $client, $params, $callback, $wait, @args); });
		}		
	} elsif ( defined $process->{cb} ) {
		$log->debug("helper stopped, processing with callback");
		$process->{cb}->($class, $client, $params, $callback, @args);
		$page = $process->{handler};
	} else {
		$page = 1;
	}
	
	if ( $page ) {
		$log->debug("updating page");
		Plugins::UPnPBridge::Squeeze2upnp->start if $prefs->get('autorun');
		$class->handler2($client, $params, $callback, @args);		  
	}
}

sub genConfig {
	my ($class, $client, $params, $callback, @args) = @_;
	my $conf = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
	
	$log->debug("lauching helper to build $conf");
	Plugins::UPnPBridge::Squeeze2upnp->start( "-i", $conf );
	waitEndHandler({ cb => undef}, $class, $client, $params, $callback, 120, @args);
}	

sub handler2 {
	my ($class, $client, $params, $callback, @args) = @_;

	if ($prefs->get('autorun')) {

		$params->{'running'}  = Plugins::UPnPBridge::Squeeze2upnp->alive;

	} else {

		$params->{'running'} = 0;
	}

	$params->{'binary'}   = Plugins::UPnPBridge::Squeeze2upnp->bin;
	$params->{'binaries'} = [ Plugins::UPnPBridge::Squeeze2upnp->binaries ];
	for my $param (@prefs_bool, @prefs_other, qw(configfile)) {	
		$params->{ $param } = $prefs->get($param);
	}
	
	$params->{'configpath'} = Slim::Utils::OSDetect::dirsFor('prefs');
	$params->{'arch'} = Slim::Utils::OSDetect::OS();
	
	my $xmlconfig = readconfig($class, KeyAttr => 'device');
		
	#load XML parameters from config file	
	if ($xmlconfig) {
	
		$params->{'devices'} = \@{$xmlconfig->{'device'}};
		unshift(@{$params->{'devices'}}, {'name' => '[common parameters]', 'udn' => '.common.'});
		
		$log->info("reading config: ", $params->{'seldevice'});
		$log->debug(dump($params->{'devices'}));
				
		#read global parameters
		for my $p (@xmlmain) {
			$params->{ $p } = $xmlconfig->{ $p };
			$log->debug("reading: ", $p, " ", $xmlconfig->{ $p });
		}
		
		# read either common parameters or device-specific
		if (!defined $params->{'seldevice'} or ($params->{'seldevice'} eq '.common.')) {
			$params->{'seldevice'} = '.common.';
			
			for my $p (@xmldevice) {
				$params->{ $p } = $xmlconfig->{'common'}->{ $p };
			}	
		} else {
			my $device = findUDN($params->{'seldevice'}, $params->{'devices'});
			
			for my $p (@xmldevice) {
				$params->{ $p } = $device->{ $p };
			}
		}
		$params->{'prevseldevice'} = $params->{'seldevice'};
		$params->{'xmlparams'} = 1;
		
		#load pre-defined list of player models	
		$params->{'updatefrom'} =  $prefs->get('profilesURL');
		my $profiles = loadprofiles();
		if ($profiles) {
		
			my @list = sort { lc($a) cmp lc($b) } (keys %$profiles);
			$params->{'devicesprofiles'} = \@list;
		}	
		
	} else {
	
		$params->{'xmlparams'} = 0;
	}
	
	$callback->($client, $params, $class->SUPER::handler($client, $params), @args);
}

sub beforeRender {
	my ($class, $params, $client) = @_;
		
	# this works only because 'mode' is set manually by reading XML	
	my @items = split /,/, $params->{mode};
	
	foreach my $item (qw(rate rate_flag flow mode size level bitrate)) {
		delete $params->{"encode_$item"};
	}	
		
	foreach my $item (@items) {
		if ($item =~ /r:(-?\d*)/) {
			$params->{encode_rate} = abs($1);
			$params->{encode_rate_flag} = 1 if ($1 < 0);
		} elsif ($item =~ /s:(\d*)/) {
			$params->{encode_size} = $1;
		} elsif ($item =~ /flow/) {	
			$params->{encode_flow} = 1;
		} else {
			$item =~ m|([^:]+):*(\d*)|i;
			$params->{encode_mode} = $1;
			$params->{encode_level} = $2 if defined $2 && $1 eq 'flc';
			$params->{encode_bitrate} = $2 if $2 && $1 eq 'mp3';
		}	
	}	
}

sub loadprofiles {
	my $file = catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'profiles.xml');
	
	if (!-e $file) {
		return undef;
	}	
	
	my $xmldata = XMLin($file, KeyAttr => 'name', ForceArray => ['profile'], KeepRoot => 0, NoAttr => 0);
	my $profiles = $xmldata->{'profile'};
	my $profileslist;
	my $root;
				
	$root = $xmldata;
	delete $root->{'profile'};
	
	foreach my $name (keys %$profiles) {
		my $father = $root;
		delete $father->{'profile'};
		setprofilenode(\$profileslist, $father, $profiles->{ $name }, $name);
	}	
	
	return $profileslist;
}

sub setprofilenode {
	my ($main, $father, $self, $name) = @_;
	my $merge = {%$father, %$self};
	my $childs = $self->{'profile'};
	delete $merge->{'profile'};
	
	if ($childs) {
		foreach my $childname (keys %$childs) {
			my $father = $merge;
			my $_name = $name . ($childname ? (" - " . $childname) : "");
			setprofilenode($main, $father, $childs->{$childname}, $_name);
		}	
	} else {
		delete $merge->{'profile'};
		${$main}->{$name} = $merge;
	}
}

sub findUDN {
	my $udn = shift(@_);
	my $listpar = shift(@_);
	my @list = @{$listpar};
	
	while (@list) {
		my $p = pop @list;
		if ($p->{ 'udn' } eq $udn) { return $p; }
	}
	return undef;
}


sub readconfig {
	my ($class,@args) = @_;
	my $ret;
	
	my $file = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
	if (-e $file) {
		$ret = XMLin($file, ForceArray => ['device'], KeepRoot => 0, NoAttr => 1, SuppressEmpty => 1, @args);
	}	
	return $ret;
}

1;
