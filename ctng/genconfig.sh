set -e
CTNG=/opt/ctng/bin/ct-ng
cd /home/build
$CTNG arm-cortex_a15-linux-gnueabihf >/dev/null 2>&1
flip(){ # flip <symbol> on, disabling the currently-selected member of its choice prefix
  local on="$1" prefix="$2"
  sed -i -E "s/^(${prefix}[0-9_]+)=y/# \1 is not set/" .config
  if grep -q "^# ${on} is not set" .config; then sed -i "s/^# ${on} is not set/${on}=y/" .config
  else grep -q "^${on}=y" .config || echo "${on}=y" >> .config; fi
}
# 1) unlock obsolete/experimental versions
for f in CT_EXPERIMENTAL CT_OBSOLETE; do
  if grep -q "^# ${f} is not set" .config; then sed -i "s/^# ${f} is not set/${f}=y/" .config; else echo "${f}=y">>.config; fi
done
$CTNG olddefconfig >/dev/null 2>&1
# 2) pin the era versions
flip CT_GCC_V_6        CT_GCC_V_
flip CT_GLIBC_V_2_12_1 CT_GLIBC_V_
flip CT_BINUTILS_V_2_26 CT_BINUTILS_V_
grep -q '^CT_CC_LANG_CXX=y' .config || echo 'CT_CC_LANG_CXX=y' >> .config
sed -i -E 's#^CT_PREFIX_DIR=.*#CT_PREFIX_DIR="/home/build/x-tools/${CT_TARGET}"#' .config
echo 'CT_LOG_PROGRESS_BAR=n' >> .config
$CTNG olddefconfig >/dev/null 2>&1
# 3) pin kernel headers to device 3.x line if offered
if grep -qE '^# CT_LINUX_V_3_16 is not set' .config; then flip CT_LINUX_V_3_16 CT_LINUX_V_; $CTNG olddefconfig >/dev/null 2>&1; fi
echo "=== RESOLVED ==="
grep -E '^CT_TARGET=|^CT_ARCH_CPU=|^CT_ARCH_FLOAT=|^CT_BINUTILS_VERSION=|^CT_GCC_VERSION=|^CT_GLIBC_VERSION=|^CT_LINUX_VERSION=|^CT_CC_LANG_CXX=' .config
cp .config /work/ctng/desay.config
