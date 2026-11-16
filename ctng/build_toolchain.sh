install -o build -g build -m 0644 /work/ctng/desay.config /home/build/.config
su build -c 'cd /home/build && /opt/ctng/bin/ct-ng olddefconfig >/dev/null 2>&1 && /opt/ctng/bin/ct-ng build.'"$(nproc)"
rc=$?
echo "ct-ng build rc=$rc"
# export whatever was installed (gcc/glibc install before any optional step)
if [ -d /home/build/x-tools ]; then
  tar -C /home/build -cf /work/x-tools.tar x-tools && echo "TARBALLED x-tools ($(du -h /work/x-tools.tar|cut -f1))"
fi
