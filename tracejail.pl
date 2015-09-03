#! /usr/bin/perl
use POSIX;
use Fcntl;
use File::Basename;
use Getopt::Long qw(:config bundling no_ignore_case require_order);
my $DTRUSS = -x "/usr/bin/dtruss";

sub usage (;$) {
    print STDERR "Usage: tracejail.pl [-o OUTFILE] COMMAND ARG...\n";
    print STDERR "       tracejail.pl [-o OUTFILE] < STRACERESULT\n";
    print STDERR "Options are:\n";
    print STDERR "  -n, --no-defaults     Do not include default programs.\n";
    print STDERR "  -o, --output=OUTFILE  Add to files listed in OUTFILE.\n";
    print STDERR "  -u, --user=USER       Don't exclude USER dotfiles.\n";
    print STDERR "  -f, --file=FILE       Include files in FILE (one per line).\n";
    print STDERR "  -V, --verbose         Be verbose.\n";
    print STDERR "Example:\n";
    print STDERR "  jail/tracejail.pl -o class/XXX/jfiles.txt sh -c \"cd ~/cs61-psets/pset5; make\"\n";
    exit (@_ ? $_[0] : 1);
}

sub run_trace (@) {
    pipe IN, OUT;
    pipe ERRIN, ERROUT;

    my($pid) = fork();
    if ($pid == 0) {
        close(IN);
        close(ERRIN);
        fcntl(OUT, F_GETFD, $buffer) or die;
        fcntl(OUT, F_SETFD, $BUFFER & ~FD_CLOEXEC) or die;
        open(STDOUT, ">&ERROUT");
        my(@args);
        if ($DTRUSS) {
            my($dir) = dirname($0);
            die "missing $dir/stderrtostdout" if !-x "$dir/stderrtostdout";
            @args = ("/usr/bin/dtruss", "-f", "-l",
                     "$dir/stderrtostdout");
        } else {
            @args = ("/usr/bin/strace", "-e", "trace=file,process",
                     "-q", "-f", "-o", "/dev/fd/" . fileno(OUT));
        }
        print STDERR join(" ", @args, @_), "\n" if $verbose;
        open(STDERR, $DTRUSS ? ">&OUT" : ">&ERROUT");
        exec @args, @_ or die;
    }
    close(OUT);
    close(ERROUT);
    open(STDIN, "<&IN");

    $pid = fork();
    if ($pid == 0) {
        close(STDIN);
        while (<ERRIN>) {
            print STDERR if !m,^\[ Process PID=\d+ runs in \d+ bit mode\. \]\s*$, || $verbose;
        }
        exit(0);
    }
    close(ERRIN);
}

my($jailfile, $defaults, $nodefaults, $help) = (undef, undef, 0, 0);
my($user) = undef;
my(@extras);
GetOptions("output|o=s" => \$jailfile,
           "defaults" => \$defaults,
           "n" => \$nodefaults,
           "help" => \$help,
           "user|u=s" => \$user,
           "file|f=s" => \@extras,
           "verbose|V" => \$verbose);
usage(0) if $help;
usage(1) if defined($defaults) && $defaults && $nodefaults;
$jailfile = undef if $jailfile eq "-";
my($userdir) = undef;
if (defined($user)) {
    $userdir = (getpwnam($user))[7];
    usage(1) if !$userdir || !-d $userdir;
    $userdir =~ s,/+\z,,;
}

if (@ARGV > 0 && $ARGV[0] =~ /\A-/) {
    usage();
}

my(%files, %allfiles, %execfiles);

if (defined($jailfile) && open(FOO, "<", $jailfile)) {
    while (<FOO>) {
        chomp;
        my($f, $v);
        if (/^.* <- (.*)$/) {
            ($f, $v) = ($1, 1);
        } elsif (/^(\S+) \[(.*)\]$/) {
            ($f, $v) = ($1, $2);
            $v = 1 if $f eq $v;
        } elsif (/\S/) {
            ($f, $v) = ($_, 1);
        } else {
            next;
        }
        $f =~ s,/+,/,g;
        $files{$f} = $allfiles{$f} = $v;
    }
    close FOO;
}

sub shquote ($) {
    my($t) = @_;
    $t =~ s,','"'"',g;
    return $t;
}

sub addfile ($$$) {
    my($inf, $op, $always) = @_;
    my($isexec) = substr($op, 0, 4) eq "exec";
    my($f, $first_line, $t, $lnk);
    $inf =~ s,/+,/,g;
    return if exists($allfiles{$inf}) && (!$isexec || exists($execfiles{$inf}));
    return if $op eq "exec.optional" && !-f $inf;
    $inf =~ s,\\0\z,,;

    $f = $inf;
    $f =~ s,/\.(?:=/|\z),,g;
    while ($f =~ m,\A(.*?/)([^./][^/]*?|[.][^./]|[.][.][^/]+?)/\.\.(/.*|)\z,
           && -d "$1$2"
           && (!-l "$1$2"
               || (defined($lnk = readlink("$1$2"))
                   && $lnk !~ m,/,
                   && -d "$1$lnk" && !-l "$1$lnk"))) {
        $f = $1 . substr($3, 1);
    }
    $f =~ s,/\z,,;
    return if $f eq "";
    return if exists($allfiles{$f}) && (!$isexec || exists($execfiles{$f}));

    print STDERR ($f eq $inf ? "ADD $f\n" : "ADD $f <= $inf\n") if $verbose;
    $allfiles{$f} = $allfiles{$inf} = 1;
    if ($always
        || $f !~ m,\A/(?:home/|tmp/|proc/|sys/|dev/pts/.|Users/|(?:etc/)?ld\.so\.conf\.d/kernel),
        || ($userdir && $f =~ m,\A$userdir/[.],)) {
        $files{$f} = 1;
    }

    # special handling required for executables
    if ($isexec && open(EXEC, "<", $f)) {
        $first_line = <EXEC>;
        close EXEC;
        if ($first_line =~ m,\A\#\!\s+(/\S+),) { # shell script
            addfile($1, "exec", 0);
        } elsif (open(EXEC, "-|", "/usr/bin/ldd", $f)) {
            print STDERR "LDD $f\n" if $verbose;
            while (defined($t = <EXEC>)) {
                addfile($1, "open", 0) if $t =~ m,\s(/\S+)\s,;
            }
            close EXEC;
        }
        $execfiles{$f} = $execfiles{$inf} = 1;
    }
}

sub readsimple (*) {
    my($fh) = @_;
    my($t);
    while (defined($t = <$fh>)) {
        if ($t =~ m,\A\s*(\S+)\s*(\S*)\s*\z,) {
            addfile($1, $2 eq "" ? "open" : $2, 1);
        }
    }
}

if (!$nodefaults && (!defined($defaults) || $defaults)) {
    readsimple DATA;
}
foreach my $fn (@extras) {
    open(F, "<", $fn) || die;
    readsimple F;
    close F;
}

if (@ARGV) {
    run_trace(@ARGV);
    my($program) = `which "$ARGV[0]"`;
    chomp $program;
    addfile($program, "exec", 0) if $program;
}

my(%pids, %tidhead, %cwd, $n, $na);
$n = $na = 0;
my($cwd) = `pwd`;
chomp $cwd;

sub resolve ($$) {
    my($f, $pid) = @_;
    if ($f !~ m,\A/,) {
        $f = $cwd{$tidhead{$pid}} . "/" . $f;
        $f =~ s,//,/,g;
    }
    $f;
}

while (defined($_ = <STDIN>)) {
    print STDERR $_ if $verbose;
    s/\A\[pid\s*(.*?)\]\s*/$1 /s;
    if (!/\A\d/) {
        next if ($DTRUSS && m,PID.*SYSCALL,) || !m,\S,;
        print STDERR "no PID: ", $_ if !$verbose;
        $_ = "0 $_";
    }
    if ($DTRUSS) {
        s,\A(\d+)/0x\w+:\s+,\1 ,;
    }
    my($pid) = int($_);

    ++$n;
    if (exists $pids{$pid}) {
        s,^\d+\s+,,;
        $_ = $pids{$pid} . $_;
        s/\s*<unfinished.*resumed>\s*//s;
        delete $pids{$pid};
        ++$na;
    }
    if (/^\d+(.*) <unfinished \.\.\.>$/s) {
        $pids{$pid} = $pid . $1;
        next;
    }

    $tidhead{$pid} = $pid if !exists $tidhead{$pid};
    $cwd{$tidhead{$pid}} = $cwd if !exists $cwd{$tidhead{$pid}};

    if (/^\d+\s*clone\(.*CLONE_THREAD.*\)\s*=\s*(\d+)/) {
        $tidhead{$1} = $pid;
        print STDERR "THREAD $1 <= $pid\n" if $verbose;
    } elsif (/^\d+\s*vfork\(\)\s*=\s*(\d+)/
             || /^\d+\s*clone\(.*\)\s*=\s*(\d+)/) {
        $tidhead{$1} = $1;
        $cwd{$1} = $cwd{$tidhead{$pid}};
        print STDERR "FORK $1 <= $pid\n" if $verbose;
    } elsif (/^\d+\s*chdir\("(.*)"\)\s*=\s*0/) {
        $cwd{$tidhead{$pid}} = resolve($1, $pid);
        print STDERR "CHDIR ", $tidhead{$pid}, " $cwd{$tidhead{$pid}}\n"
            if $verbose;
    } elsif (/^\d*\s*(access|chdir|chmod|exec[a-z]+|lstat|open|openat|readlink|stat\d*|statfs|truncate|unlink|unlinkat|utimensat)\(\s*"(.*?)".*\)\s*=\s*([0-9]|-1 \S+)/s) {
        addfile(resolve($2, $pid), $1, 0) if $3 ne "-1 ENOENT";
    } elsif ($verbose && !/^\d*\s*(?:wait4|exit_group|vfork|arch_prctl|mkdir|---|\+\+\+)/) {
        print STDERR "CONFUSION ", $_;
    }
}

if (defined($jailfile)) {
    open(STDOUT, ">", $jailfile) || die;
}
@files = sort { $a cmp $b } keys %files;
foreach $_ (@files) {
    my $x = $files{$_};
    print $_, ($x == 1 ? "\n" : " [$x]\n");
}

__DATA__
/bin/bash exec
/bin/cat exec
/bin/chmod exec
/bin/false exec
/bin/kill exec
/bin/ls exec
/bin/mv exec
/bin/ps exec
/bin/rm exec
/bin/rmdir exec
/bin/sed exec.optional
/bin/true exec
/dev/null
/dev/urandom
/etc/group
/etc/ld.so.conf
/etc/ld.so.conf.d
/etc/nsswitch.conf
/etc/passwd
/etc/profile
/usr/bin/head exec
/usr/bin/id exec
/usr/bin/ldd exec
/usr/bin/sed exec.optional
/usr/bin/tail exec
/usr/bin/test exec
/usr/bin/tr exec
/usr/lib/pt_chown exec.optional
