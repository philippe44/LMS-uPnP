package Plugins::UPnPBridge::Squeeze2upnp;

use strict;

use Proc::Background;
use File::ReadBackwards;
use File::Spec::Functions;
use Data::Dumper;

use Slim::Utils::Log;
use Slim::Utils::Prefs;
use XML::Simple;

my $prefs = preferences('plugin.upnpbridge');
my $log   = logger('plugin.upnpbridge');

my $squeeze2upnp;
my $binary;

sub binaries {
	my $os = Slim::Utils::OSDetect::details();
	
	if ($os->{'os'} eq 'Linux') {

		if ($os->{'osArch'} =~ /x86_64/) {
			return qw(squeeze2upnp-linux_x86_64 squeeze2upnp-linux-x86_64-static );
		}
		if ($os->{'binArch'} =~ /i386/) {
			return qw(squeeze2upnp-linux-x86 squeeze2upnp-linux-x86-static );
		}
		if ($os->{'osArch'} =~ /aarch64/) {
			return qw(squeeze2upnp-linux-aarch64 squeeze2upnp-linux-aarch64-static );
		}
		if ($os->{'binArch'} =~ /armhf/) {
			return qw(squeeze2upnp-linux-arm squeeze2upnp-linux-arm-static squeeze2upnp-linux-armv6 squeeze2upnp-linux-armv6-static);
		}
		if ($os->{'binArch'} =~ /powerpc/) {
			return qw(squeeze2upnp-linux-powerpc squeeze2upnp-linux-powerpc-static );
		}
		if ($os->{'binArch'} =~ /sparc/) {
			return qw(squeeze2upnp-linux-sparc64 squeeze2upnp-linux-sparc64-static );
		}
		if ($os->{'binArch'} =~ /mips/) {
			return qw(squeeze2upnp-linux-mips squeeze2upnp-linux-mips-static );
		}		
		
	}
	
	if ($os->{'os'} eq 'Unix') {
	
		if ($os->{'osName'} eq 'solaris') {
			return qw(squeeze2upnp-solaris-x86_64 squeeze2upnp-solaris-x86_64-static );
		}	
		if ($os->{'osName'} =~ /freebsd/) {
			return qw( squeeze2upnp-freebsd-x86_64 squeeze2upnp-freebsd-x86_64-static );
		}
		
	}	
	
	if ($os->{'os'} eq 'Darwin') {
		
		if ($os->{'osArch'} =~ /x86_64/) {
			return qw(squeeze2upnp-macos-x86_64 squeeze2upnp-macos-x86_64-static);
		}
		if ($os->{'osArch'} =~ /arm64/) {
			return qw(squeeze2upnp-macos-am64 squeeze2upnp-macos-arm64-static);
		}
		
	}
	
	if ($os->{'os'} eq 'Windows') {
		return qw(squeeze2upnp.exe squeeze2upnp-static.exe);
	}	
	
}

sub bin {
	my $class = shift;
	my @binaries = $class->binaries;
	my $bin = $prefs->get("bin");

	return grep($bin, @binaries) ? $bin : @binaries;
}

sub start {
	my $class = shift;

	my $bin = $class->bin || do {
		$log->warn("no binary set");
		return;
	};

	my @params;
	my $logging;

	push @params, ("-Z");
	
	if ($prefs->get('autosave')) {
		push @params, ("-I");
	}
	
	if ($prefs->get('eraselog')) {
		unlink $class->logFile;
	}
	
	if ($prefs->get('useLMSsocket')) {
		my $binding = Slim::Utils::Network::serverAddr;
		$binding .= ':' . $prefs->get('baseport') if $prefs->get('baseport');
		push @params, ("-b", $binding);
	}

	if ($prefs->get('logging')) {
		push @params, ("-f", $class->logFile);
		
		if ($prefs->get('debugs') ne '') {
			push @params, ("-d", $prefs->get('debugs') . "=debug");
		}
		$logging = 1;
	}
	
	if (-e $class->configFile || $prefs->get('autosave')) {
		push @params, ("-x", $class->configFile);
	}
	
	if ($prefs->get('opts') ne '') {
		push @params, split(/\s+/, $prefs->get('opts'));
	}
	
	if (Slim::Utils::OSDetect::details()->{'os'} ne 'Windows') {
		my $exec = catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'Bin', $bin);
		$exec = Slim::Utils::OSDetect::getOS->decodeExternalHelperPath($exec);
			
		if (!((stat($exec))[2] & 0100)) {
			$log->warn('executable not having \'x\' permission, correcting');
			chmod (0555, $exec);
		}	
	}	
	
	my $path = Slim::Utils::Misc::findbin($bin) || do {
		$log->warn("$bin not found");
		return;
	};

	my $path = Slim::Utils::OSDetect::getOS->decodeExternalHelperPath($path);
			
	if (!-e $path) {
		$log->warn("$bin not executable");
		return;
	}
	
	push @params, @_;

	if ($logging) {
		open(my $fh, ">>", $class->logFile);
		print $fh "\nStarting Squeeze2upnp: $path @params\n";
		close $fh;
	}
	
	eval { $squeeze2upnp = Proc::Background->new({ 'die_upon_destroy' => 1 }, $path, @params); };

	if ($@) {

		$log->warn($@);

	} else {
		Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 1, sub {
			if ($squeeze2upnp && $squeeze2upnp->alive) {
				$log->debug("$bin running");
				$binary = $path;
			}
			else {
				$log->debug("$bin NOT running");
			}
		});
		
		Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 30, \&beat, $path, @params);
	}
}

sub beat {
	my ($class, $path, @args) = @_;
	
	if ($prefs->get('autorun') && !($squeeze2upnp && $squeeze2upnp->alive)) {
		$log->error('crashed ... restarting');
		
		if ($prefs->get('logging')) {
			open(my $fh, ">>", $class->logFile);
			print $fh "\nRetarting Squeeze2upnp after crash: $path @args\n";
			close $fh;
		}
		
		eval { $squeeze2upnp = Proc::Background->new({ 'die_upon_destroy' => 1 }, $path, @args); };
	}	
	
	Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 30, \&beat, $path, @args);
}

sub stop {
	my $class = shift;

	if ($squeeze2upnp && $squeeze2upnp->alive) {
		$log->info("killing squeeze2upnp");
		$squeeze2upnp->die;
	}
}

sub alive {
	return ($squeeze2upnp && $squeeze2upnp->alive) ? 1 : 0;
}

sub wait {
	$log->info("waiting for squeeze2upnp to end");
	$squeeze2upnp->wait
}

sub restart {
	my $class = shift;

	$class->stop;
	$class->start;
}

sub logFile {
	return catdir(Slim::Utils::OSDetect::dirsFor('log'), "upnpbridge.log");
}

sub configFile {
	return catdir(Slim::Utils::OSDetect::dirsFor('prefs'), $prefs->get('configfile'));
}

sub logHandler {
	my ($client, $params, undef, undef, $response) = @_;

	$response->header("Refresh" => "10; url=" . $params->{path} . ($params->{lines} ? '?lines=' . $params->{lines} : ''));
	$response->header("Content-Type" => "text/plain; charset=utf-8");

	my $body = '';
	my $file = File::ReadBackwards->new(logFile());
	
	if ($file){

		my @lines;
		my $count = $params->{lines} || 1000;

		while ( --$count && (my $line = $file->readline()) ) {
			unshift (@lines, $line);
		}

		$body .= join('', @lines);

		$file->close();			
	};

	return \$body;
}

sub configHandler {
	my ($client, $params, undef, undef, $response) = @_;

	$response->header("Content-Type" => "text/xml; charset=utf-8");

	my $body = '';
	
	if (-e configFile) {
		open my $fh, '<', configFile;
		
		read $fh, $body, -s $fh;
		close $fh;
	}	

	return \$body;
}

sub guideHandler {
	my ($client, $params) = @_;
		
	return Slim::Web::HTTP::filltemplatefile('plugins/UPnPBridge/userguide.htm', $params);
}

1;
