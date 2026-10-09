#!/usr/bin/perl
# vnecho.pl <host> <port> <megabytes>: sends that much through a TCP echo server and checks that
# the same bytes come back, in order. A child writes, the parent reads. Perl 5.6 (10.0) and on: no
# module beyond Socket.
use Socket;
my ($host, $port, $mb) = @ARGV;
my $total = $mb * 1024 * 1024;

# A deterministic stream, 64 KiB at a time: every byte value, shifted by the block's number.
sub block { my $n = shift; my $b = ''; for (my $i = 0; $i < 256; $i++) { $b .= chr(($i + $n) % 256) } return $b x 256; }

socket(S, PF_INET, SOCK_STREAM, getprotobyname('tcp')) or die "socket: $!";
connect(S, sockaddr_in($port, inet_aton($host))) or die "connect: $!";
select((select(S), $| = 1)[0]);
my $t0 = time;
my $pid = fork;
die "fork: $!" unless defined $pid;
if ($pid == 0) {
    for (my $n = 0; $n * 65536 < $total; $n++) {
        my $b = block($n);
        my $off = 0;
        while ($off < length $b) {
            my $w = syswrite(S, $b, length($b) - $off, $off);
            die "write: $!" unless defined $w;
            $off += $w;
        }
    }
    shutdown(S, 1);
    exit 0;
}
my ($got, $buf, $want) = (0, '', '');
my $n = 0;
while ($got < $total) {
    my $r = sysread(S, $buf, 65536);
    die "read: $!" unless defined $r;
    die "connection closed after $got of $total bytes" if $r == 0;
    $want .= block($n++) while length($want) < $r;
    die "bytes differ near offset $got" if substr($want, 0, $r) ne $buf;
    $want = substr($want, $r);
    $got += $r;
}
waitpid($pid, 0);
die "writer failed" if $?;
my $dt = time - $t0;
$dt = 1 if $dt < 1;
printf "%d bytes back intact in %d s, %d KB/s each way\n", $got, $dt, $got / 1024 / $dt;
exit 0;
