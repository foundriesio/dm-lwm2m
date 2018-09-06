These files are used in addition to the top-level prj.conf (or other
future top-level config files), NOT instead of it.

The build system finds board files in this directory based on the
BOARD setting. There's no need to modify CONF_FILE or DTC_OVERLAY_FILE
to include these configurations in the build.
