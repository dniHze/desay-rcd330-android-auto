#!/bin/sh
# Android Auto launcher for the system install (busybox 1.22.1 sh).
#
#   aa_run.sh carlife|receiver|bridge [--debug-output MODE]
#
# carlife: the stock init.sh starts /svp/bin/carlifeservice under procguard in the CarLife slot,
# about 4 s into boot, before appmgr starts AppConnect. In the system install that file is a
# two-line stub that runs this script. On the first start of a boot it decides the mode:
#   aa     bind-mounts the rebranded AppConnect files (each guarded by md5), starts the receiver
#          and the bridge under their own procguard, then execs aa-carlife-shim;
#   stock  execs the stock CarLife service (carlifeservice.stock, run under its own name).
# Later starts (procguard restarts a service 50 ms after exit 0 or a crash, with no limit)
# reuse the boot's mode. More than AA_MAX_STARTS starts within 60 s: the stock service takes the
# slot for the rest of the boot, and the receiver and the bridge are stopped.
# receiver, bridge: configuration to environment, then exec. Repeated crashes back off.
#
# Stock is the answer to anything unexpected: AA switched off in the config or by an "aa-off"
# file on a card or stick, a binary missing, or boots that keep dying early (boot-loop guard).
# Logs go to syslog (/var/log/messages, tags aa-run, aa-carlife-shim, aa-receiver, aa-bridge):
# the shim writes there itself, and the output of the receiver and the bridge (and anything
# stray from the shim) goes through the shim's non-blocking relay (aa-carlife-shim
# --log-relay), which drops lines rather than ever stalling them. Decisions also go to
# $STATE/events.log, which survives a reboot.
#
# The configuration is read as KEY=VALUE lines, never sourced or eval'd: /svp/etc/aa.conf
# (defaults), then $STATE/aa.conf (overrides, optional).
#
# For a rehearsal from the SD card (start.sh option "installed") the caller sets AA_ROOT (the
# copied /svp tree), AA_STATE_DIR (on the card) and AA_STOCK_BIN (/svp/bin/carlifeservice).

set -f # no globbing: nothing here expands file names
SVC=${1:-}
[ $# -gt 0 ] && shift
ROOT=${AA_ROOT:-/svp}
RUN=${AA_RUN_DIR:-/tmp/run/aa}
STATE=${AA_STATE_DIR:-/storage/data/data/aa}
STOCK=${AA_STOCK_BIN:-$ROOT/bin/carlifeservice.stock}
BIN=$ROOT/bin
CTL_SOCK=/tmp/run/aa-carlife.sock
FATAL=$RUN/shim.fatal # written by the shim when it cannot start (ports, registry)
MEDIA="/media/sdcard0 /media/sdcard0p1 /media/sdcard1 /media/hub_sdcard0 /media/hub_sdcard1
	/media/usbstorage0 /media/usbstorage1 /media/usbstorage2
	/media/hub_usbstorage0 /media/hub_usbstorage1 /media/hub_usbstorage2"

up() { cut -d' ' -f1 /proc/uptime; }
upsecs() { cut -d. -f1 /proc/uptime; }

# To syslog (never the console: under procguard stderr is the 115200-baud serial port).
say() { logger -t aa-run "$SVC: $*" 2> /dev/null; }

# A decision worth keeping across reboots: syslog and $STATE/events.log (64 KB, one rotation).
event() {
	say "$*"
	mkdir -p "$STATE" 2> /dev/null || return 0
	f=$STATE/events.log
	if [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 65536 ]; then mv -f "$f" "$f.1"; fi
	echo "boot $(cat "$STATE/boots" 2> /dev/null) up $(up) $SVC: $*" >> "$f"
}

# Small state files: written to a temporary name, synced, then renamed over the old one.
put() {
	mkdir -p "$STATE" 2> /dev/null || return 1
	echo "$2" > "$STATE/.$1.tmp" && sync && mv -f "$STATE/.$1.tmp" "$STATE/$1"
}

number() {
	case "$1" in '' | *[!0-9]*) echo "${2:-0}" ;; *) echo "$1" ;; esac
}

# KEY=VALUE lines. Keys: AA_* and DEBUG, except the paths above, which only the caller sets.
# Values: letters, digits and _ . , : / = + - only. Anything else is logged and skipped.
conf_load() {
	[ -f "$1" ] || return 0
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in '' | '#'*) continue ;; esac
		k=${line%%=*}
		v=${line#*=}
		if [ "$k" = "$line" ]; then
			say "$1: line without '=' skipped"
			continue
		fi
		case "$k" in
		AA_ROOT | AA_RUN_DIR | AA_STATE_DIR | AA_STOCK_BIN | AA_CTL_SOCK | AA_SHIM_NOTIFY | AA_SHIM_FATAL_FILE | \
			AA_LAST_MODE_FILE | AA_BOOTS_FILE | AA_SHIM_SYSLOG_SOCK | AA_RECEIVER_COMM | AA_BRIDGE_COMM | \
			AA_SHIM_FAKE* | AA_CL_* | AA_*_HOST | AA_SESSIONS_DIR | AA_HELPER_SOCK | AA_VIDEO_SOCK | AA_MIC_SOCK)
			# Paths, test modes and addresses: fixed by this script or for tests only.
			say "$1: $k cannot be set here; skipped"
			continue
			;;
		*[!A-Z0-9_]*) say "$1: key $k skipped"; continue ;;
		AA_* | DEBUG) ;;
		*) say "$1: key $k skipped"; continue ;;
		esac
		case "$v" in
		*[!A-Za-z0-9_.,:/=+-]*) say "$1: $k has characters outside A-Za-z0-9_.,:/=+-; skipped"; continue ;;
		esac
		export "$k=$v"
	done < "$1"
}

# stdout and stderr of this shell (and of what it execs) to syslog with tag $1, through a
# FIFO and the shim's relay (busybox logger if the shim is not there, as for the stock
# service's fallback). AA_LOG=console keeps them (the serial console), AA_LOG=none drops them.
# --debug-output console (the unit's debug.flag) means console too.
log_to() {
	case "$LOG" in
	console) return 0 ;;
	none)
		exec > /dev/null 2>&1
		return 0
		;;
	esac
	f=$RUN/$1.fifo
	rm -f "$f"
	relay="$BIN/aa-carlife-shim --log-relay $1"
	[ -x "$BIN/aa-carlife-shim" ] || relay="logger -t $1"
	if ! command -v ${relay%% *} > /dev/null 2>&1 || ! mkfifo "$f" 2> /dev/null; then
		exec > /dev/null 2>&1
		return 0
	fi
	# The reader opens first in the background; the open for writing below waits for it.
	setsid $relay < "$f" > /dev/null 2>&1 &
	exec > "$f" 2>&1
	rm -f "$f"
}

# One instance per service: the lock stays with fd 9 into the exec'd binary. A child of a
# process that just died can hold it a moment longer, so it is retried for up to 15 s.
lock_or_wait() {
	exec 9> "$RUN/$1.lock"
	i=0
	while ! flock -n 9; do
		i=$((i + 1))
		if [ $i -eq 30 ]; then
			say "another $1 holds $RUN/$1.lock; trying again later"
			exit 0
		fi
		usleep 500000
	done
}

# Starts within the last 60 s, this one included.
starts() {
	f=$RUN/$1.starts
	now=$(upsecs)
	{ cat "$f" 2> /dev/null; echo "$now"; } | while read -r t; do
		t=$(number "$t" "$now")
		[ $((now - t)) -lt 60 ] && echo "$t"
	done > "$f.new"
	mv -f "$f.new" "$f"
	wc -l < "$f" | tr -d ' '
}

# procguard'd children of this launcher (receiver, bridge), started like svp_start_service.
start_guarded() {
	group=$2
	cmd="sh $ROOT/etc/aa_run.sh $1 --debug-output $DBG"
	if [ -x /svp/bin/procguard ] && [ "${SVP_ENABLE_PROCGUARD:-true}" = true ]; then
		cmd="/svp/bin/procguard $cmd"
	fi
	if [ -x /usr/bin/cgexec ] && [ "${SVP_ENABLE_CGROUP:-true}" = true ] && [ -n "$group" ]; then
		cmd="/usr/bin/cgexec -g cpu:$group $cmd"
	fi
	# No redirection to the console: each child sends its own output to syslog.
	$cmd < /dev/null > /dev/null 2>&1 &
	say "$1 started under procguard (cgroup ${group:-none})"
}

stop_guarded() {
	for pid in $(pidof procguard); do
		tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null | grep -q "aa_run.sh $1 " && kill "$pid"
	done
	killall "$2" 2> /dev/null
}

# The rebrand: $ROOT/etc/aa_rebrand.md5 lines "<stock md5> <rebranded md5> <source> <target>",
# source relative to $ROOT. A target is replaced only while AppConnect is not running yet,
# only if it is the stock file the source was made from, and only if the source is intact.
rebrand() {
	spec=$ROOT/etc/aa_rebrand.md5
	[ -f "$spec" ] || return 0
	if pidof appconnect > /dev/null; then
		event "rebrand skipped: AppConnect is already running"
		return 0
	fi
	while read -r want new src target; do
		case "$want" in '' | '#'*) continue ;; esac
		have=$(md5sum "$target" 2> /dev/null | cut -d' ' -f1)
		[ "$have" = "$new" ] && continue # already in place
		got=$(md5sum "$ROOT/$src" 2> /dev/null | cut -d' ' -f1)
		if [ "$have" != "$want" ]; then
			event "rebrand: $target is not the stock file ($have); left alone"
		elif [ "$got" != "$new" ]; then
			event "rebrand: $ROOT/$src is damaged ($got); $target left alone"
		elif mount --bind "$ROOT/$src" "$target"; then
			say "rebrand: $target <- $src"
		else
			event "rebrand: mounting $src over $target failed"
		fi
	done < "$spec"
}

# "aa-off" / "aa-on" at the root of a card or stick, at boot or any time later: only file
# names are looked at, nothing on the media is read or run. aa-off switches AA off from the next boot; aa-on switches it back on
# and clears the boot-loop counter. aa-off wins if both are there.
markers() {
	for m in $MEDIA; do
		if [ -e "$m/aa-off" ]; then
			if [ ! -e "$STATE/disabled" ]; then
				put disabled "aa-off on $m" && event "aa-off on $m: Android Auto is off from the next boot"
			fi
			return 0
		fi
	done
	for m in $MEDIA; do
		if [ -e "$m/aa-on" ]; then
			if [ -e "$STATE/disabled" ] || [ "$(number "$(cat "$STATE/boots" 2> /dev/null)")" -gt 1 ]; then
				rm -f "$STATE/disabled"
				put boots 1 && event "aa-on on $m: Android Auto is on, boot-loop counter cleared"
			fi
			return 0
		fi
	done
}

# The UI language (our libUIPublic.so, in every app, AA on or off): "aa-lang-off" at the root of a
# card or stick sets it to "off" (the stock texts and language list, the car's code choosing
# Chinese or English), and so does AA_LANG=0 in the configuration (lang_conf, at boot);
# "aa-lang-on" sets it back to English. Nothing on the media is read. The state is a 32-byte
# record every app maps: it is rewritten in place ("1<>", no truncation), never replaced, since
# a mapped file that shrinks faults the apps reading it.
lang_code() {
	line=
	read -r line < "$STATE/lang" 2> /dev/null
	case "$line" in "AALANG1 "*) set -- $line && echo "$2" ;; esac
}
lang_put() {
	mkdir -p "$STATE" 2> /dev/null || return 1
	[ -e "$STATE/lang" ] || : > "$STATE/lang"
	printf 'AALANG1 %-23s\n' "$1" 1<> "$STATE/lang" && sync
}
lang_markers() {
	for m in $MEDIA; do
		if [ -e "$m/aa-lang-off" ]; then
			[ "$(lang_code)" = off ] || { lang_put off && event "aa-lang-off on $m: UI language off (stock texts)"; }
			return 0
		fi
	done
	for m in $MEDIA; do
		if [ -e "$m/aa-lang-on" ]; then
			[ "$(lang_code)" = off ] && lang_put en && event "aa-lang-on on $m: UI language on (English)"
			return 0
		fi
	done
}
lang_conf() {
	if [ "${AA_LANG:-1}" = 0 ] && [ "$(lang_code)" != off ]; then
		lang_put off && event "AA_LANG=0: UI language off (stock texts)"
	fi
}

# Settings > Appearance (our libUIPublic.so: the colour theme and the start screen):
# "aa-appearance-off" at the root of a card or stick, or AA_APPEARANCE=0 in the configuration,
# sets both back to Automatic, the car's coding (the launcher applies it at its next start).
# Nothing on the media is read. The choice is a 32-byte record, rewritten in place as the
# language's is.
appearance_auto() {
	line=
	read -r line < "$STATE/appearance" 2> /dev/null
	case "$line" in "" | "AAAPPR1 T=auto L=auto"*) return 0 ;; esac
	mkdir -p "$STATE" 2> /dev/null || return 1
	printf 'AAAPPR1 %-23s\n' "T=auto L=auto" 1<> "$STATE/appearance" && sync && event "$1: colour and start screen back to the coding"
}
appearance_markers() {
	for m in $MEDIA; do
		if [ -e "$m/aa-appearance-off" ]; then
			appearance_auto "aa-appearance-off on $m"
			return 0
		fi
	done
}
appearance_conf() {
	[ "${AA_APPEARANCE:-1}" = 0 ] && appearance_auto "AA_APPEARANCE=0"
	return 0
}

# Logs for bug reports, onto a card or stick: only one with an "aa-logs" folder at its root (opt-in;
# nothing on the media is read or run). Each boot has its own folder, aa-logs/log<NNNN>, copied at
# the insertion and again every AA_LOG_REFRESH_SECS (default 60; 0: once per insertion) while the
# media stays in and the syslog grew, so it holds the latest without taking the card out. Bounded:
# the unit's syslog is the stock 5 MB with one rotation (syslogd -s 5120, in RAM), a rotated file
# is copied only when it changed, events.log is 64 KB with one rotation, and only the newest
# AA_LOG_KEEP folders (default 10) stay on the media: older log<NNNN> folders are removed. The unit
# mounts cards and sticks read-only: the media is remounted read-write for the copy only.
log_name() { # this boot's folder: one past the unit's counter and every log<NNNN> on the media
	f=$RUN/logname
	if [ ! -s "$f" ]; then
		n=$(number "$(cat "$STATE/logseq" 2> /dev/null)")
		set +f
		for x in "$1"/aa-logs/log[0-9][0-9][0-9][0-9]; do
			x=${x##*/log}
			x=${x#"${x%%[!0]*}"}
			[ -n "$x" ] && [ "$(number "$x")" -gt "$n" ] && n=$x
		done
		set -f
		n=$((n + 1))
		put logseq "$n"
		printf 'log%04d\n' "$n" > "$f"
	fi
	cat "$f"
}
export_logs() {
	refresh=$(number "${AA_LOG_REFRESH_SECS:-}" 60)
	keep=$(number "${AA_LOG_KEEP:-}" 10)
	[ "$keep" -ge 1 ] || keep=1
	now=$(upsecs)
	size=$(wc -c < /var/log/messages 2> /dev/null)
	for m in $MEDIA; do
		flag=$RUN/exported.${m##*/} # "<uptime of the last copy> <syslog size then>"
		if [ ! -d "$m/aa-logs" ]; then
			rm -f "$flag"
			continue
		fi
		first=1
		if [ -f "$flag" ]; then
			first=0
			last= lastsize=
			read -r last lastsize < "$flag"
			[ "$refresh" -gt 0 ] || continue
			[ $((now - $(number "$last"))) -ge "$refresh" ] || continue
			[ "$size" != "$lastsize" ] || continue
		fi
		echo "$now $size" > "$flag"
		d=$m/aa-logs/$(log_name "$m")
		ro=0
		if ! mkdir -p "$d" 2> /dev/null; then
			mount -o rw,remount "$m" 2> /dev/null && ro=1
			if ! mkdir -p "$d" 2> /dev/null; then
				[ $first = 1 ] && event "logs: cannot write to $m"
				[ $ro = 1 ] && mount -o ro,remount "$m" 2> /dev/null
				continue
			fi
		fi
		rm -f "$d/DONE"
		set +f
		cp /var/log/messages "$d/" 2> /dev/null
		for f in /var/log/messages.[0-9]*; do
			[ -f "$f" ] || continue
			[ "$(wc -c < "$f")" = "$(wc -c < "$d/${f##*/}" 2> /dev/null)" ] || cp "$f" "$d/" 2> /dev/null
		done
		cp "$STATE"/events.log* "$STATE/lastmode" "$STATE/boots" "$ROOT/etc/aa_version" "$d/" 2> /dev/null
		for f in "$RUN"/*; do
			[ -f "$f" ] && echo "== $f" && cat "$f"
		done > "$d/run-aa.txt" 2>&1
		set -f
		dmesg > "$d/dmesg.txt" 2>&1
		ps > "$d/ps.txt" 2>&1
		df -k > "$d/df.txt" 2>&1
		{
			echo "build: $(head -n 1 "$ROOT/etc/aa_version" 2> /dev/null)"
			echo "boot: $(cat "$STATE/boots" 2> /dev/null) ($(cat /proc/sys/kernel/random/boot_id 2> /dev/null))"
			echo "copied at uptime $now s; refreshed every $refresh s while the media stays in"
		} > "$d/info.txt"
		: > "$d/DONE"
		if [ $first = 1 ]; then
			event "logs copied to $d"
			cp "$STATE/events.log" "$d/" 2> /dev/null # with that line
		fi
		# The newest $keep log<NNNN> folders stay (this boot's among them).
		set +f
		ls -d "$m"/aa-logs/log[0-9][0-9][0-9][0-9] 2> /dev/null | sort -r | tail -n +$((keep + 1)) | while read -r x; do
			[ "$x" = "$d" ] || rm -rf "$x"
		done
		set -f
		sync
		[ $ro = 1 ] && mount -o ro,remount "$m" 2> /dev/null
	done
}

# Background, detached: the media watcher (every 2 s for 2 minutes, then every 5 s), and the
# boot-loop counter cleared once this boot has lasted AA_STABLE_SECS.
background() {
	stable=$(number "${AA_STABLE_SECS:-}" 90)
	setsid sh -c '
		i=0
		while :; do
			sh "$0" markers-check
			if [ $i -lt 60 ]; then sleep 2; else sleep 5; fi
			i=$((i + 1))
		done' "$ROOT/etc/aa_run.sh" < /dev/null > /dev/null 2>&1 &
	setsid sh -c '
		while [ "$(cut -d. -f1 /proc/uptime)" -lt "$1" ]; do sleep 5; done
		sh "$0" stable' "$ROOT/etc/aa_run.sh" "$stable" < /dev/null > /dev/null 2>&1 &
}

decide() {
	[ "${AA_ENABLE:-1}" = 1 ] || { echo "stock AA_ENABLE=${AA_ENABLE} in aa.conf"; return; }
	[ -e "$STATE/disabled" ] && { echo "stock switched off ($(cat "$STATE/disabled" 2> /dev/null))"; return; }
	max=$(number "${AA_BOOTLOOP_MAX:-}" 3)
	[ "$BOOTS" -gt "$max" ] && { echo "stock boot-loop guard: $((BOOTS - 1)) boots in a row ended within ${AA_STABLE_SECS:-90} s"; return; }
	for b in aa-carlife-shim aa-headless aa-bridge; do
		[ -x "$BIN/$b" ] || { echo "stock $BIN/$b missing"; return; }
	done
	echo aa
}

exec_stock() {
	if [ ! -x "$STOCK" ]; then
		say "stock CarLife service $STOCK missing; nothing serves the CarLife slot"
		exit 1 # procguard leaves the slot down
	fi
	# Its output (it prints little) never goes through our relay: the fallback must not depend
	# on our binaries.
	[ "$LOG" = console ] || exec > /dev/null 2>&1
	case "$STOCK" in
	*/carlifeservice) exec "$STOCK" "$@" ;;
	esac
	# Under its own name, through a link, as some tools look for "carlifeservice".
	mkdir -p "$RUN/stock" && ln -sf "$STOCK" "$RUN/stock/carlifeservice" && exec "$RUN/stock/carlifeservice" "$@"
	exec "$STOCK" "$@"
}

# ---- entry points without a service --------------------------------------------------------
case "$SVC" in
markers-check)
	mkdir -p "$RUN"
	markers
	lang_markers
	appearance_markers
	export_logs
	exit 0
	;;
stable)
	put boots 0
	exit 0
	;;
esac

mkdir -p "$RUN"
DBG=none
[ "${1:-}" = --debug-output ] && DBG=${2:-none}
conf_load "$ROOT/etc/aa.conf"
conf_load "$STATE/aa.conf"
LOG=${AA_LOG:-syslog}
[ "$DBG" = console ] && LOG=console
export AA_CTL_SOCK=$CTL_SOCK # always gated: the receiver switches no phone without the shim
export AA_SHIM_NOTIFY=$RUN/notify.sock AA_SHIM_FATAL_FILE=$FATAL
# The binaries take where to log from --debug-output: console means stderr, the rest syslog.
if [ "$LOG" = console ]; then
	[ "${1:-}" = --debug-output ] && shift 2
	set -- --debug-output console "$@"
	DBG=console
fi
if [ "${AA_DEBUG:-0}" = 1 ]; then
	export DEBUG=1 AA_SHIM_VERBOSE=1
	[ -n "${GST_DEBUG:-}" ] || export GST_DEBUG=2
fi

case "$SVC" in
carlife)
	if [ ! -f "$RUN/mode" ]; then
		BOOTS=$(($(number "$(cat "$STATE/boots" 2> /dev/null)") + 1))
		put boots "$BOOTS" || say "cannot write $STATE/boots"
		markers
		lang_markers
		lang_conf
		appearance_markers
		appearance_conf
		set -- $(decide) # first word the mode, the rest the reason
		MODE=$1
		shift
		echo "$MODE" > "$RUN/mode"
		event "mode $MODE${*:+: $*} ($(head -n 1 "$ROOT/etc/aa_version" 2> /dev/null))"
		background
		if [ "$MODE" = aa ]; then
			[ "${AA_REBRAND:-1}" = 1 ] && rebrand
			start_guarded receiver "${AA_CGROUP_RECEIVER-link}"
			start_guarded bridge "${AA_CGROUP_BRIDGE-svp}"
		fi
		set -- --debug-output "$DBG"
	fi
	if [ "$(cat "$RUN/mode")" = aa ] && [ -f "$FATAL" ]; then
		echo stock > "$RUN/mode"
		event "aa-carlife-shim could not start ($(head -c 200 "$FATAL")): the stock CarLife service takes over until the next boot"
		stop_guarded receiver aa-headless
		stop_guarded bridge aa-bridge
		exec_stock "$@"
	fi
	if [ "$(cat "$RUN/mode")" = aa ]; then
		n=$(starts carlife)
		if [ "$n" -gt "$(number "${AA_MAX_STARTS:-}" 5)" ]; then
			echo stock > "$RUN/mode"
			event "aa-carlife-shim started $n times within 60 s: the stock CarLife service takes over until the next boot"
			stop_guarded receiver aa-headless
			stop_guarded bridge aa-bridge
			exec_stock "$@"
		fi
		[ "$n" -gt 1 ] && say "aa-carlife-shim restart ($n within 60 s)"
		lock_or_wait carlife
		export AA_BOOTS_FILE="$STATE/boots" # a clean switch-off clears the boot-loop counter
		case "${AA_LAST_MODE:-1}" in
		0) export AA_LAST_MODE_FILE= ;;
		*) [ -n "${AA_LAST_MODE_FILE+x}" ] || export AA_LAST_MODE_FILE="$STATE/lastmode" ;;
		esac
		if [ -z "${AA_VIDEO_SOURCE:-}" ]; then
			case "${AA_VIDEO_SINK:-waylandsink2}" in
			waylandsink) export AA_VIDEO_SOURCE=8,24,792,480 ;;
			*) export AA_VIDEO_SOURCE=32,24,800,480 ;;
			esac
		fi
		mkdir -p "$STATE"
		log_to aa-carlife-shim
		exec "$BIN/aa-carlife-shim" "$@"
	fi
	exec_stock "$@"
	;;
receiver | bridge)
	[ "$(cat "$RUN/mode" 2> /dev/null)" = aa ] || {
		say "not in aa mode; $SVC stays down"
		exit 1
	}
	n=$(starts "$SVC")
	max=$(number "${AA_MAX_STARTS:-}" 5)
	if [ "$n" -gt "$max" ]; then
		event "$SVC started $n times within 60 s; waiting 30 s before the next start"
		sleep 30
	elif [ "$n" -gt 1 ]; then
		say "$SVC restart ($n within 60 s)"
		sleep 1
	fi
	lock_or_wait "$SVC"
	if [ "$SVC" = receiver ]; then
		log_to aa-receiver
		exec "$BIN/aa-headless" "$@"
	fi
	log_to aa-bridge
	# AA_VIDEO_DECODER: off the unit only (the emulator has no IVA-HD: "identity" passes the
	# H.264 on); unset, the bridge's built-in ducatih264dec.
	exec "$BIN/aa-bridge" --sink "${AA_VIDEO_SINK:-waylandsink2}" ${AA_VIDEO_DECODER:+--decoder "$AA_VIDEO_DECODER"} "$@"
	;;
*)
	say "usage: aa_run.sh carlife|receiver|bridge [--debug-output MODE]"
	exit 1
	;;
esac
