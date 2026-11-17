#!/system/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export CLASSPATH="$DIR/classLoader.apk"
unset LD_LIBRARY_PATH LD_PRELOAD
exec /system/bin/app_process -Xnoimage-dex2oat / --nice-name="vsouchi-debug" io.github.lumaparallax.vsouchi.Loader "$@"
