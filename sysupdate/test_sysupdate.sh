#!/usr/bin/env bash
# Tests of the system install's launcher (svp/etc/aa_run.sh and the carlifeservice stub) under
# busybox, as on the unit, against a fake procguard (the real one's rules: restart 50 ms after
# exit 0 or a signal, give up on any other exit code), a fake cgexec, busybox syslogd, and
# stand-ins for the three binaries, the stock CarLife service and AppConnect:
#
#   docker run --rm --privileged --platform linux/arm64 -v "$PWD":/w debian:bookworm \
#       bash /w/sysupdate/test_sysupdate.sh            (from the work dir)
#
# --privileged only for mount --bind (the rebrand). The install goes to the real paths (/svp,
# /storage/data/data/aa, /storage/app/...), so the defaults are what is tested.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
U="${W}/sysupdate"

apt-get update -qq > /dev/null && apt-get install -y -qq busybox procps > /dev/null || { echo "cannot install busybox"; exit 1; }
mkdir -p /bb && busybox --install -s /bb && rm -f /bb/pkill /bb/pgrep
export PATH=/svp/bin:/svp/etc:/bb:${PATH}

fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; [ -n "${3:-}" ] && { echo "---- $3"; cat "$3" 2> /dev/null; echo "----"; }; fi; }
# Polls up to $2 s (default 5) for condition $1.
waitfor() { local i=0; while ! eval "$1"; do sleep 0.2; i=$((i + 1)); [ $i -ge $((${2:-5} * 5)) ] && return 1; done; return 0; }

T=/tmp/t
APPC=/storage/app/sv.g5.mib2.appconnect
STATE=/storage/data/data/aa

# ---- the unit, as far as the launcher sees it -------------------------------------------------
mkdir -p /svp/bin /svp/etc /svp/share /svp/lib /usr/bin "$APPC/bin" "$APPC/res" /storage/data/data /var/log
cp "${U}/svp/etc/aa_run.sh" "${U}/svp/etc/aa.conf" /svp/etc/
cp "${U}/svp/bin/carlifeservice" /svp/bin/
echo "aa-test-1 flavour release" > /svp/etc/aa_version
# The container has been up for longer than AA_STABLE_SECS: no early counter reset (section 9
# sets its own).
echo "AA_STABLE_SECS=999999" >> /svp/etc/aa.conf

# procguard: restart after exit 0 or a signal, give up otherwise (procguard's strings).
cat > /svp/bin/procguard << 'EOF'
#!/bb/sh
echo "$$ start $*" >> /tmp/t/procguard.log
while :; do
	"$@"
	rc=$?
	if [ $rc -eq 0 ] || [ $rc -gt 128 ]; then echo "$$ restart after rc $rc: $*" >> /tmp/t/procguard.log; usleep 50000; continue; fi
	echo "$$ guard unusable (rc $rc): $*" >> /tmp/t/procguard.log
	exit 1
done
EOF
printf '#!/bb/sh\necho "$2 $3" >> /tmp/t/cgexec.log\nshift 2\nexec "$@"\n' > /usr/bin/cgexec
# The three binaries: log what they were given, then run, exit 0 or crash, as $T/<name>.do says.
for b in aa-carlife-shim aa-headless aa-bridge; do
	cat > "/svp/bin/$b" << EOF
#!/bb/sh
# The shim's log relay (aa-carlife-shim --log-relay TAG): stdin lines to syslog.
[ "\$1" = --log-relay ] && exec logger -t "\$2"
if [ "\$(cat /tmp/t/$b.do 2> /dev/null)" = fatal ]; then
	mkdir -p "\$(dirname "\$AA_SHIM_FATAL_FILE")" && echo "cannot bind 30407" > "\$AA_SHIM_FATAL_FILE"
	exit 0
fi
echo "start \$\$ args=\$* ctl=\$AA_CTL_SOCK lastmode=\${AA_LAST_MODE_FILE-unset} notify=\${AA_SHIM_NOTIFY-} fake=\${AA_SHIM_FAKE-} src=\${AA_VIDEO_SOURCE-} can=\${AA_CAN_PORT-} debug=\${DEBUG-} lock=\$(readlink /proc/\$\$/fd/9)" >> /tmp/t/$b.log
echo "$b says hello on stderr" >&2
case "\$(cat /tmp/t/$b.do 2> /dev/null)" in
crash) kill -SEGV \$\$ ;;
exit0) exit 0 ;;
esac
while :; do sleep 1; done
EOF
done
cat > /svp/bin/carlifeservice.stock << 'EOF'
#!/bb/sh
echo "stock $$ comm=$(cat /proc/$$/comm) args=$*" >> /tmp/t/stock.log
while :; do sleep 1; done
EOF
chmod 755 /svp/bin/* /usr/bin/cgexec

# AppConnect's files and their rebranded versions, with the spec the build writes.
printf 'stock appconnect\n' > /tmp/stock-appconnect
printf 'stock kzb\n' > /tmp/stock-kzb
printf 'stock lib\n' > /tmp/stock-lib
printf 'rebranded appconnect\n' > /svp/share/aa_appconnect
printf 'rebranded kzb\n' > /svp/share/aa_VW_MP2017.kzb
printf 'rebranded lib\n' > /svp/share/aa_libUIPublic.so
m() { md5sum "$1" | cut -d' ' -f1; }
{
	echo "$(m /tmp/stock-appconnect) $(m /svp/share/aa_appconnect) share/aa_appconnect $APPC/bin/appconnect"
	echo "$(m /tmp/stock-kzb) $(m /svp/share/aa_VW_MP2017.kzb) share/aa_VW_MP2017.kzb $APPC/res/VW_MP2017.kzb"
	echo "$(m /tmp/stock-lib) $(m /svp/share/aa_libUIPublic.so) share/aa_libUIPublic.so /svp/lib/libUIPublic.so"
} > /svp/etc/aa_rebrand.md5

busybox syslogd -n -O /var/log/messages &
sleep 0.5

stop_all() {
	pkill -f /svp/bin/procguard; pkill -f aa_run.sh; pkill -f 'logger -t'
	pkill -f /svp/bin/aa-; pkill -f carlifeservice; pkill -f fake-appconnect
	sleep 0.3
	for t in "$APPC/bin/appconnect" "$APPC/res/VW_MP2017.kzb" /svp/lib/libUIPublic.so; do
		while umount "$t" 2> /dev/null; do :; done
	done
}

# A boot: /tmp/run wiped (init.sh's svp_clear_tmp_data), stock AppConnect files, then the
# CarLife slot as svp_start_service starts it. $1 = "keep" keeps $STATE.
boot() {
	stop_all
	rm -rf /tmp/run "$T" && mkdir -p /tmp/run/service/registry "$T"
	[ "${1:-}" = keep ] || rm -rf "$STATE"
	cp /tmp/stock-appconnect "$APPC/bin/appconnect"
	cp /tmp/stock-kzb "$APPC/res/VW_MP2017.kzb"
	cp /tmp/stock-lib /svp/lib/libUIPublic.so
	: > /var/log/messages
	(cd /svp/bin && nice -n -7 /usr/bin/cgexec -g cpu:link /svp/bin/procguard carlifeservice --debug-output none \
		< /dev/null > "$T/console.log" 2>&1 &)
}
running() { pgrep -f "/svp/bin/$1" > /dev/null; }
count() { grep -c "$2" "$1" 2> /dev/null || echo 0; }

echo "== 1. scripts parse under busybox sh"
for f in /svp/etc/aa_run.sh /svp/bin/carlifeservice; do
	check "sh -n $(basename "$f")" "busybox sh -n '$f'"
done

echo "== 2. first boot: aa mode"
boot
check "the shim runs" "waitfor 'running aa-carlife-shim'" /var/log/messages
check "the receiver runs" "waitfor 'running aa-headless'"
check "the bridge runs" "waitfor 'running aa-bridge'"
check "mode aa" "[ \"\$(cat /tmp/run/aa/mode)\" = aa ]"
check "boot counter 1" "[ \"\$(cat $STATE/boots)\" = 1 ]"
check "events.log: mode aa with the version" "grep -q 'mode aa (aa-test-1 flavour release)' $STATE/events.log" "$STATE/events.log"
check "the shim's environment: gated, last mode, video source, config" \
	"grep -q 'ctl=/tmp/run/aa-carlife.sock lastmode=$STATE/lastmode notify=/tmp/run/aa/notify.sock fake= src=32,24,800,480 can=30705 debug=' $T/aa-carlife-shim.log" "$T/aa-carlife-shim.log"
check "the shim got --debug-output and holds its lock" "grep -q 'args=--debug-output none .*lock=/tmp/run/aa/carlife.lock' $T/aa-carlife-shim.log" "$T/aa-carlife-shim.log"
check "the receiver is gated too" "grep -q 'ctl=/tmp/run/aa-carlife.sock' $T/aa-headless.log" "$T/aa-headless.log"
check "the bridge gets its sink" "grep -q 'args=--sink waylandsink2 --debug-output none' $T/aa-bridge.log" "$T/aa-bridge.log"
check "cgroups: link for the slot and the receiver, svp for the bridge" \
	"[ \"\$(sort $T/cgexec.log | tr '\n' ' ')\" = 'cpu:link /svp/bin/procguard cpu:link /svp/bin/procguard cpu:svp /svp/bin/procguard ' ]" "$T/cgexec.log"
check "rebrand: all three files replaced" \
	"grep -q rebranded $APPC/bin/appconnect && grep -q rebranded $APPC/res/VW_MP2017.kzb && grep -q rebranded /svp/lib/libUIPublic.so"
check "stderr of each binary reaches syslog with its tag" \
	"waitfor 'grep -q \"aa-carlife-shim: aa-carlife-shim says hello\" /var/log/messages && grep -q \"aa-receiver: aa-headless says hello\" /var/log/messages && grep -q \"aa-bridge: aa-bridge says hello\" /var/log/messages'" /var/log/messages
check "nothing on the console" "[ ! -s $T/console.log ]" "$T/console.log"

echo "== 3. a shim crash: restarted, nothing started twice"
pkill -SEGV -f /svp/bin/aa-carlife-shim
check "the shim runs again" "waitfor '[ \$(count $T/aa-carlife-shim.log start) -eq 2 ] && running aa-carlife-shim'" "$T/aa-carlife-shim.log"
sleep 0.5
check "one receiver, one bridge" "[ \$(count $T/aa-headless.log start) -eq 1 ] && [ \$(count $T/aa-bridge.log start) -eq 1 ]"
check "the rebrand is mounted once" "[ \$(grep -c ' $APPC/bin/appconnect ' /proc/mounts) -eq 1 ]"

echo "== 4. the shim keeps crashing: the stock service takes over"
echo crash > "$T/aa-carlife-shim.do"
pkill -SEGV -f /svp/bin/aa-carlife-shim
check "the stock service runs under its own name" "waitfor 'grep -q \"comm=carlifeservice args=--debug-output none\" $T/stock.log' 10" "$T/procguard.log"
check "events.log says why" "grep -q 'started 6 times within 60 s: the stock CarLife service takes over' $STATE/events.log" "$STATE/events.log"
check "the receiver and the bridge are stopped" "waitfor '! running aa-headless && ! running aa-bridge'"
check "and stay stopped" "sleep 1; ! running aa-headless && ! running aa-bridge"
check "mode is stock for the rest of the boot" "[ \"\$(cat /tmp/run/aa/mode)\" = stock ]"

echo "== 5. the receiver keeps crashing: it backs off, the shim is not affected"
boot
waitfor 'running aa-headless'
echo crash > "$T/aa-headless.do"
pkill -SEGV -f /svp/bin/aa-headless
check "after 6 starts it waits 30 s" "waitfor 'grep -q \"receiver started 6 times within 60 s; waiting 30 s\" $STATE/events.log' 15" "$STATE/events.log"
check "the shim runs on" "running aa-carlife-shim && [ \$(count $T/aa-carlife-shim.log start) -eq 1 ]"

echo "== 6. switched off in the config: stock, nothing of ours"
boot
mkdir -p "$STATE" && echo "AA_ENABLE=0" > "$STATE/aa.conf"
boot keep
check "the stock service runs" "waitfor 'grep -q comm=carlifeservice $T/stock.log'" "$T/procguard.log"
check "no receiver, no bridge, no shim" "sleep 1; ! running aa-headless && ! running aa-bridge && ! running aa-carlife-shim"
check "no rebrand" "grep -q stock $APPC/bin/appconnect && ! grep -q ' $APPC/bin/appconnect ' /proc/mounts"
check "events.log says why" "grep -q 'mode stock: AA_ENABLE=0 in aa.conf' $STATE/events.log" "$STATE/events.log"

echo "== 7. the override file: bad lines skipped, good ones applied"
cat > "$STATE/aa.conf" << 'EOF'
AA_CAN_PORT=0
AA_ROOT=/tmp/evil
LD_PRELOAD=/tmp/evil.so
AA_X=$(reboot)
AA_Y=a b
AA_SHIM_FAKE=1
no equals sign
# a comment
AA_DEBUG=1
EOF
boot keep
waitfor 'running aa-carlife-shim'
check "AA_CAN_PORT=0 and AA_DEBUG=1 applied" "grep -q 'can=0 debug=1' $T/aa-carlife-shim.log" "$T/aa-carlife-shim.log"
check "AA_ROOT, LD_PRELOAD, a command and a space refused" \
	"grep -q 'AA_ROOT cannot be set here' /var/log/messages && grep -q 'key LD_PRELOAD skipped' /var/log/messages && grep -q 'AA_X has characters' /var/log/messages && grep -q 'AA_Y has characters' /var/log/messages" /var/log/messages
check "nothing ran from the bad lines" "[ ! -e /tmp/evil ] && running aa-carlife-shim"
check "test modes cannot be switched on" "grep -q 'AA_SHIM_FAKE cannot be set here' /var/log/messages && grep -q 'fake= ' $T/aa-carlife-shim.log" "$T/aa-carlife-shim.log"
rm -f "$STATE/aa.conf"

echo "== 8. a binary missing: stock"
mv /svp/bin/aa-bridge /tmp/aa-bridge.away
boot
check "stock, and why" "waitfor 'grep -q comm=carlifeservice $T/stock.log' && grep -q 'mode stock: /svp/bin/aa-bridge missing' $STATE/events.log" "$STATE/events.log"
mv /tmp/aa-bridge.away /svp/bin/aa-bridge

echo "== 9. boot-loop guard"
boot
waitfor 'running aa-carlife-shim'
echo 3 > "$STATE/boots"
echo "AA_STABLE_SECS=999999" > "$STATE/aa.conf"
boot keep
check "the 4th early boot in a row runs stock" "waitfor 'grep -q comm=carlifeservice $T/stock.log' && grep -q 'boot-loop guard: 3 boots in a row' $STATE/events.log" "$STATE/events.log"
echo "AA_STABLE_SECS=$(($(cut -d. -f1 /proc/uptime) + 3))" > "$STATE/aa.conf"
boot keep
check "a boot that lasts clears the counter" "waitfor '[ \"\$(cat $STATE/boots)\" = 0 ]' 15" "$STATE/boots"
boot keep
check "and the next boot is aa again" "waitfor 'running aa-carlife-shim' && [ \"\$(cat /tmp/run/aa/mode)\" = aa ]"
rm -f "$STATE/aa.conf"

echo "== 10. aa-off / aa-on on a card"
mkdir -p /media/sdcard0 && touch /media/sdcard0/aa-off
boot
check "aa-off at boot: stock at once" "waitfor 'grep -q comm=carlifeservice $T/stock.log' && [ -e $STATE/disabled ]" "$STATE/events.log"
rm /media/sdcard0/aa-off
boot keep
check "stays off without the card" "waitfor 'grep -q comm=carlifeservice $T/stock.log'"
touch /media/sdcard0/aa-on
check "aa-on seen during the boot clears it" "waitfor '[ ! -e $STATE/disabled ]' 10 && grep -q 'aa-on on /media/sdcard0' $STATE/events.log" "$STATE/events.log"
rm /media/sdcard0/aa-on
boot keep
check "next boot: aa" "waitfor 'running aa-carlife-shim'"
touch /media/sdcard0/aa-off
check "aa-off inserted later: off from the next boot" "waitfor '[ -e $STATE/disabled ]' 10 && running aa-carlife-shim" "$STATE/events.log"
rm /media/sdcard0/aa-off
rm -f "$STATE/disabled"

echo "== 11. rebrand guards"
boot
waitfor 'running aa-carlife-shim'
stop_all
rm -rf /tmp/run "$T" && mkdir -p /tmp/run/service/registry "$T"
cp /tmp/stock-appconnect "$APPC/bin/appconnect"
echo "an AppConnect from another version" > "$APPC/res/VW_MP2017.kzb"
cp /tmp/stock-lib /svp/lib/libUIPublic.so
cp /svp/share/aa_libUIPublic.so /tmp/lib.good && echo "damaged" > /svp/share/aa_libUIPublic.so
(cd /svp/bin && /svp/bin/procguard carlifeservice --debug-output none > /dev/null 2>&1 &)
waitfor 'running aa-carlife-shim'
check "a target that is not stock is left alone" "grep -q 'another version' $APPC/res/VW_MP2017.kzb && grep -q 'VW_MP2017.kzb is not the stock file' $STATE/events.log" "$STATE/events.log"
check "a damaged source is not mounted" "grep -q stock /svp/lib/libUIPublic.so && grep -q 'aa_libUIPublic.so is damaged' $STATE/events.log" "$STATE/events.log"
check "the good one is" "grep -q rebranded $APPC/bin/appconnect"
cp /tmp/lib.good /svp/share/aa_libUIPublic.so
stop_all
rm -rf /tmp/run "$T" && mkdir -p /tmp/run/service/registry "$T"
cp /tmp/stock-appconnect "$APPC/bin/appconnect"
cp /tmp/stock-kzb "$APPC/res/VW_MP2017.kzb"
printf '#!/bb/sh\nwhile :; do sleep 1; done\n' > /tmp/appconnect && chmod 755 /tmp/appconnect
/tmp/appconnect &
APPC_PID=$!
(cd /svp/bin && /svp/bin/procguard carlifeservice --debug-output none > /dev/null 2>&1 &)
waitfor 'running aa-carlife-shim'
check "AppConnect already running: no rebrand, but AA runs" \
	"grep -q stock $APPC/bin/appconnect && grep -q 'rebrand skipped: AppConnect is already running' $STATE/events.log" "$STATE/events.log"
kill $APPC_PID

echo "== 12. one instance per service"
boot
waitfor 'running aa-carlife-shim'
(cd /svp/bin && sh /svp/etc/aa_run.sh carlife --debug-output none > /dev/null 2>&1 &)
check "a second launcher waits 15 s for the lock, then leaves" "waitfor 'grep -q \"another carlife holds\" /var/log/messages' 20" /var/log/messages
check "and starts no second shim" "[ \$(count $T/aa-carlife-shim.log start) -eq 1 ]" "$T/aa-carlife-shim.log"

echo "== 13. log settings"
mkdir -p "$STATE" && echo "AA_LOG=none" > "$STATE/aa.conf"
boot keep
waitfor 'running aa-carlife-shim'
sleep 0.5
check "AA_LOG=none: nothing from the binaries in syslog, nothing on the console" \
	"! grep -q 'says hello' /var/log/messages && [ ! -s $T/console.log ]"
echo "AA_LOG=syslog" > "$STATE/aa.conf"
stop_all
rm -rf /tmp/run "$T" && mkdir -p /tmp/run/service/registry "$T"
(cd /svp/bin && /svp/bin/procguard carlifeservice --debug-output console > "$T/console.log" 2>&1 &)
check "--debug-output console (the unit's debug.flag): the console" "waitfor 'grep -q \"aa-carlife-shim says hello\" $T/console.log'" "$T/console.log"
rm -f "$STATE/aa.conf"

echo "== 14. the shim cannot start (ports taken): stock at once"
boot
mkdir -p "$T" && echo fatal > "$T/aa-carlife-shim.do"
check "the stock service takes the slot" "waitfor 'grep -q comm=carlifeservice $T/stock.log' 10" "$T/procguard.log"
check "events.log has the shim's reason" "grep -q 'could not start (cannot bind 30407)' $STATE/events.log" "$STATE/events.log"
check "the receiver and the bridge are stopped" "waitfor '! running aa-headless && ! running aa-bridge'"

echo "== 15. AA_LOG=console: the binaries are told so"
mkdir -p "$STATE" && echo "AA_LOG=console" > "$STATE/aa.conf"
boot keep
check "the shim gets --debug-output console" "waitfor 'grep -q \"args=--debug-output console\" $T/aa-carlife-shim.log'" "$T/aa-carlife-shim.log"
check "the bridge too" "waitfor 'grep -q \"args=--sink waylandsink2 --debug-output console\" $T/aa-bridge.log'" "$T/aa-bridge.log"
rm -f "$STATE/aa.conf"

echo "== 16. an aa-logs folder on a card: this boot's folder (log<NNNN>), refreshed while the card stays in, the newest 10 kept"
rm -rf "$STATE" && mkdir -p "$STATE" && echo "AA_LOG_REFRESH_SECS=2" > "$STATE/aa.conf" # the watcher inherits it from the boot
boot keep
waitfor 'running aa-carlife-shim'
L=/media/sdcard0/aa-logs
mkdir -p $L/log0005 $L/boot1-up7 # an older folder of ours, and one of the builds before
check "this boot's folder, one past the card's highest: syslog, events.log, run state, info, DONE" \
	"waitfor 'ls $L/log0006/DONE > /dev/null 2>&1' 10 && grep -q 'mode aa' $L/log0006/events.log && [ -s $L/log0006/messages ] && grep -q '== /tmp/run/aa/mode' $L/log0006/run-aa.txt && grep -q '^build: ' $L/log0006/info.txt" "$STATE/events.log"
logger -t test "after the first copy"
check "refreshed while the card stays in, once the syslog grew" "waitfor 'grep -q \"after the first copy\" $L/log0006/messages' 10"
check "one folder per boot" "[ \$(ls -d $L/log* | wc -l) -eq 2 ]"
check "events.log says where, once per insertion" "[ \$(grep -c 'logs copied to $L/log0006' $STATE/events.log) -eq 1 ]"
mv $L /tmp/aa-logs.out
sleep 3
mv /tmp/aa-logs.out $L
check "taken out and put back: copied into the same folder again" \
	"waitfor '[ \$(grep -c \"logs copied to $L/log0006\" $STATE/events.log) -eq 2 ]' 10 && [ \$(ls -d $L/log* | wc -l) -eq 2 ]"
for n in 01 02 03 04 07 08 09 10 11 12; do mkdir -p $L/log00$n; done
logger -t test "more"
check "only the newest 10 log folders stay (this boot's among them); others are left alone" \
	"waitfor '[ \$(ls -d $L/log* | wc -l) -eq 10 ]' 10 && [ -d $L/log0006 ] && [ -d $L/log0012 ] && [ ! -d $L/log0002 ] && [ -d $L/log0003 ] && [ -d $L/boot1-up7 ]" "$STATE/events.log"
rm -rf $L
# As on the unit: the card mounted read-only.
mkdir -p /media/sdcard1 && mount -t tmpfs tmpfs /media/sdcard1 && mkdir /media/sdcard1/aa-logs && mount -o ro,remount /media/sdcard1
check "a read-only card: remounted for the copy, read-only again after" \
	"waitfor 'ls /media/sdcard1/aa-logs/log*/DONE > /dev/null 2>&1' 10 && sleep 1 && grep -q ' /media/sdcard1 tmpfs ro' /proc/mounts && grep -q 'logs copied to /media/sdcard1' /media/sdcard1/aa-logs/log*/events.log" /proc/mounts
umount /media/sdcard1
rm -f "$STATE/aa.conf"

echo "== 17. receiver and bridge refuse to run outside aa mode"
stop_all
rm -rf /tmp/run "$T" && mkdir -p /tmp/run/aa "$T" && echo stock > /tmp/run/aa/mode
sh /svp/etc/aa_run.sh receiver --debug-output none
check "exit 1 (procguard leaves it down)" "[ $? -eq 1 ] && ! running aa-headless"

echo "== 18. the UI language switch: aa-lang-off / aa-lang-on cards, AA_LANG=0"
rec() { printf 'AALANG1 %-23s\n' "$1"; }
boot
mkdir -p "$STATE" && rec uk > "$STATE/lang"
ino=$(stat -c %i "$STATE/lang")
mkdir -p /media/usbstorage0 && : > /media/usbstorage0/aa-lang-off
check "an aa-lang-off stick: the state becomes off" "waitfor '[ \"\$(cat $STATE/lang)\" = \"\$(rec off)\" ]' 10" "$STATE/lang"
check "written in place: same file, 32 bytes" "[ \$(stat -c %i $STATE/lang) = $ino ] && [ \$(stat -c %s $STATE/lang) = 32 ]"
check "events.log says so" "grep -q 'aa-lang-off on /media/usbstorage0: UI language off' $STATE/events.log" "$STATE/events.log"
rm /media/usbstorage0/aa-lang-off && : > /media/usbstorage0/aa-lang-on
check "an aa-lang-on stick: English again" "waitfor '[ \"\$(cat $STATE/lang)\" = \"\$(rec en)\" ]' 10" "$STATE/lang"
rm /media/usbstorage0/aa-lang-on
sleep 3
check "a chosen language is left alone by aa-lang-on" "rec de > $STATE/lang && : > /media/usbstorage0/aa-lang-on && sleep 3 && [ \"\$(cat $STATE/lang)\" = \"\$(rec de)\" ]"
rm -f /media/usbstorage0/aa-lang-on
echo "AA_LANG=0" > "$STATE/aa.conf"
boot keep
check "AA_LANG=0 at boot: off" "waitfor '[ \"\$(cat $STATE/lang)\" = \"\$(rec off)\" ]' 10" "$STATE/lang"
rm -f "$STATE/aa.conf" "$STATE/lang"
boot keep
check "no state file and no switch: the launcher leaves it to the apps" "sleep 2; [ ! -e $STATE/lang ]"

echo "== 18b. Settings > Appearance's switch: aa-appearance-off card, AA_APPEARANCE=0"
app() { printf 'AAAPPR1 %-23s\n' "$1"; }
app "T=auto L=auto" > /tmp/app-auto
app "T=red L=3" > "$STATE/appearance"
ino=$(stat -c %i "$STATE/appearance")
: > /media/usbstorage0/aa-appearance-off
check "an aa-appearance-off stick: both back to the coding" "waitfor 'cmp -s $STATE/appearance /tmp/app-auto' 10" "$STATE/appearance"
check "written in place: same file, 32 bytes" "[ \$(stat -c %i $STATE/appearance) = $ino ] && [ \$(stat -c %s $STATE/appearance) = 32 ]"
check "events.log says so" "grep -q 'aa-appearance-off on /media/usbstorage0: colour and start screen back to the coding' $STATE/events.log" "$STATE/events.log"
rm /media/usbstorage0/aa-appearance-off
app "T=blue L=auto" > "$STATE/appearance"
echo "AA_APPEARANCE=0" > "$STATE/aa.conf"
boot keep
check "AA_APPEARANCE=0 at boot: both back to the coding" "waitfor 'cmp -s $STATE/appearance /tmp/app-auto' 10" "$STATE/appearance"
rm -f "$STATE/aa.conf" "$STATE/appearance"
boot keep
check "no choice file: none made" "sleep 2; [ ! -e $STATE/appearance ]"

echo "== 19. The shim's stub: aa-carlife-shim execs aa-headless shim, same process, same arguments"
S=/tmp/stub && rm -rf $S && mkdir -p $S
cp "${U}/svp/bin/aa-carlife-shim" $S/ && chmod 755 $S/aa-carlife-shim
printf '#!/bin/sh\necho "pid=$$ args=$*"\n' > $S/aa-headless && chmod 755 $S/aa-headless
out=$(sh -c 'echo "outer=$$"; exec "$0" --debug-output none' $S/aa-carlife-shim)
outer=$(echo "$out" | sed -n 's/^outer=//p')
inner=$(echo "$out" | sed -n 's/^pid=\([0-9]*\) .*/\1/p')
check "the stub passes its arguments after 'shim'" 'echo "$out" | grep -q "args=shim --debug-output none"'
check "and keeps its process (exec)" '[ -n "$outer" ] && [ "$outer" = "$inner" ]'

stop_all
echo
echo "${fails} failure(s)"
exit $((fails > 0))
