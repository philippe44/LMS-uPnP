package Plugins::UPnPBridge::Settings;

use strict;

use File::Spec::Functions;
#use File::Fetch;
use LWP::Simple;
use base qw(Slim::Web::Settings);
use XML::Simple;
use Data::Dumper;
use Slim::Utils::PluginManager;
use Slim::Utils::Prefs;
use Slim::Utils::Log;


my $prefs = preferences('plugin.upnpbridge');
my $log   = logger('plugin.upnpbridge');
my @xmlmain = qw(upnp_socket upnp_scan_interval upnp_scan_timeout);
my @xmldevice = qw(name mac stream_length accept_nexturi seek_after_pause buffer_dir buffer_limit sample_rate codecs L24_format flac_header enabled upnp_remove_count send_metadata volume_on_play max_volume);

sub name { 'PLUGIN_UPNPBRIDGE' }

sub page { 'plugins/UPnPBridge/settings/basic.html' }

sub handler {
	my ($class, $client, $params, $callback, @args) = @_;

	my $update;
			
	if ($params->{'updateprofiles'}) {			
	
		$log->debug("update profile required: ", $params->{'updatefrom'});
		
		$prefs->set('profilesURL', $params->{'updatefrom'});		
		my $content = get($prefs->get('profilesURL'));
		if ($content) {
			open(my $fh, ">", catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'profiles.xml')); 
			print $fh $content;
			close $fh;
		}
		
		#my $ff = File::Fetch->new($prefs->get('profilesURL'));
		#$ff->fetch( to => catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'profiles.xml')); 
		
		#okay, this is hacky, will change in the future, just don't want another indent layer :-(
		$params->{'saveSettings'} = 0;
	}
	
	if ($params->{ 'delconfig' }) {
				
		require Plugins::UPnPBridge::Squeeze2upnp;
		my $conf = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
		unlink $conf;							
		$log->info("deleting configuration $conf");
		
		#okay, this is hacky, will change in the future, just don't want another indent layer :-(
		$params->{'saveSettings'} = 0;
		
		$update = 1;
	}
		
	if ($params->{ 'genconfig' }) {
			
		require Plugins::UPnPBridge::Squeeze2upnp;
		my $conf = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
		Plugins::UPnPBridge::Squeeze2upnp->stop;
		Plugins::UPnPBridge::Squeeze2upnp->start( "-i", $conf );
		Plugins::UPnPBridge::Squeeze2upnp->wait;
		$log->info("generation configuration $conf");
		
		#okay, this is hacky, will change in the future, just don't want another indent layer :-(
		$params->{'saveSettings'} = 0;
		
		$update = 1;
	}
	
	if ($params->{'saveSettings'}) {

		$log->debug("save settings required");
		
		my @bool  = qw(autorun logging autosave);
		my @other = qw(output bin debugs opts);
		my $skipxml;
				
		for my $param (@bool) {
			
			my $val = $params->{ $param } ? 1 : 0;
			
			if ($val != $prefs->get($param)) {
					
				$prefs->set($param, $val);
				$update = 1;
				
				if ($param eq 'autorun') {
					require Plugins::UPnPBridge::Squeeze2upnp;
				}
	
			}
		}
		
		# check that the config file name has not changed first
		for my $param (@other) {
		
			if ($params->{ $param } ne $prefs->get($param)) {
			
				$prefs->set($param, $params->{ $param });
				$update = 1;
			}
		}
		
		if ($params->{ 'configfile' } ne $prefs->get('configfile')) {
		
			$prefs->set('configfile', $params->{ 'configfile' });
			$update = 1;
			$skipxml = 1;
		}	
		
		my $xmlconfig = readconfig($class, KeyAttr => 'device');
				
		# get XML player configuration if current device has changed in the list
		if ($xmlconfig and !$skipxml and ($params->{'seldevice'} eq $params->{'prevseldevice'})) {
		
			$log->info('Writing XML:', $params->{'seldevice'});
			for my $p (@xmlmain) {
				
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
					if ($params->{ $p } eq '') {
						delete $xmlconfig->{ common }->{ $p };
					} else {
						$xmlconfig->{ common }->{ $p } = $params->{ $p };
					}
				}	
				
			# save player specific parameters
			} else {
			
				$params->{'devices'} = \@{$xmlconfig->{'device'}};
				my $device = findUDN($params->{'seldevice'}, $params->{'devices'});
								
				for my $p (@xmldevice) {
					if ($params->{ $p } eq '') {
						delete $device->{ $p };
					} else {
						$device->{ $p } = $params->{ $p };
					}
				}	
				
				# a profile has been applied, then overwrite some of the above
				if ($params->{ 'applyprofile' }) {
			
					my $profile = loadprofiles()->{ $params->{'selprofile'} };
					mergeprofile($device, $profile);
				}	
			}
			
			# get enabled status for all device, except the selected one
			foreach my $device (@{$xmlconfig->{'device'}}) {
				if ($device->{'udn'} ne $params->{'seldevice'}) {
					my $enabled = $params->{ 'enabled.'.$device->{ 'udn' } };
					$device->{'enabled'} = defined $enabled ? $enabled : 0;
				}	
			}
			
			$log->info("writing XML config");
			Plugins::UPnPBridge::Squeeze2upnp->stop;
			XMLout($xmlconfig, RootName => "squeeze2upnp", NoSort => 1, NoAttr => 1, OutputFile => Plugins::UPnPBridge::Squeeze2upnp->configFile($class));
			$update = 1;
			$log->debug(Dumper($xmlconfig));
		}	
	}

	# something has been updated, XML array is up-to-date anyway, but need to write it
	if ($update) {

		$log->debug("updating");
				
		$prefs->get('autorun') ? Plugins::UPnPBridge::Squeeze2upnp->restart : Plugins::UPnPBridge::Squeeze2upnp->stop;

		Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 1, sub {
			$class->handler2( $client, $params, $callback, @args);		  
		});

	#no update detected or first time looping
	} else {

		$log->debug("not updating");
		$class->handler2( $client, $params, $callback, @args);		  
	}

	return undef;
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
	for my $param (qw(autorun output bin opts debugs logging configfile autosave)) {
		$params->{ $param } = $prefs->get($param);
	}
	
	$params->{'configpath'} = Slim::Utils::OSDetect::dirsFor('prefs');
	$params->{'arch'} = Slim::Utils::OSDetect::OS();
	
	my $xmlconfig = readconfig($class, KeyAttr => 'device');
		
	#load XML parameters from config file	
	if ($xmlconfig) {
	
		$params->{'devices'} = \@{$xmlconfig->{'device'}};
		unshift(@{$params->{'devices'}}, {'name' => '[default parameters]', 'udn' => '.common.'});
		
		$log->info("reading config: ", $params->{'seldevice'});
		$log->debug(Dumper($params->{'devices'}));
				
		#read global parameters
		for my $p (@xmlmain) {
			#$params->{ $p } = defined $xmlconfig->{ $p } ? $xmlconfig->{ $p } : 'default';
			$params->{ $p } = $xmlconfig->{ $p };
			$log->debug("reading: ", $p, " ", $xmlconfig->{ $p });
		}
		
		# read either common parameters or device-specific
		if (!defined $params->{'seldevice'} or ($params->{'seldevice'} eq '.common.')) {
			$params->{'seldevice'} = '.common.';
			
			for my $p (@xmldevice) {
				#$params->{ $p } = defined $xmlconfig->{common}->{ $p } ? $xmlconfig->{common}->{ $p } : '';
				$params->{ $p } = $xmlconfig->{common}->{ $p };
			}	
		} else {
			my $device = findUDN($params->{'seldevice'}, $params->{'devices'});
			
			for my $p (@xmldevice) {
				#$params->{ $p } = defined $device->{ $p } ? $device->{ $p } : '';
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

sub mergeprofile{
	my ($p1, $p2) = @_;
	
	foreach my $m (keys %$p2) {
		$p1->{ $m } = $p2-> { $m };
	}	
}

sub loadprofiles {
	my $file = catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'profiles.xml');
	
	if (!-e $file) {
		return undef;
	}	
	
	my $xmlprofiles = XMLin($file, KeyAttr => 'name', ForceArray => ['profile'], KeepRoot => 0, NoAttr => 0)->{'profile'};
	my $profileslist;
					
	foreach my $name (keys %$xmlprofiles) {
		my $node;
		undef $node;
		setprofilenode(\$profileslist, \$node, $xmlprofiles->{ $name }, $name);
	}	
	
	$log->debug("dumping profiles:\n", Dumper($profileslist));
	return $profileslist;
}


sub setprofilenode {
	my ($main, $root, $node, $name) = @_;
	my $child;
	
	if (${$root}) {
		${$root} = {%${$root}, %$node};
	} else {
		${$root} = $node;
	}
	
	$child = $node->{'profile'};
	if (!$child) {
		${$main}->{ $name } = ${$root};
		delete ${$main}->{ $name }->{'profile'};
	} else {
		foreach my $childname (keys %$child) {
			my $concat = $childname ? (" - " . $childname) : "";
			setprofilenode($main, $root, $child->{ $childname }, $name . $concat);
		}	
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
	
	require Plugins::UPnPBridge::Squeeze2upnp;
	my $file = Plugins::UPnPBridge::Squeeze2upnp->configFile($class);
	if (-e $file) {
		$ret = XMLin($file, ForceArray => ['device'], KeepRoot => 0, NoAttr => 1, @args);
	}	
	return $ret;
}

1;
