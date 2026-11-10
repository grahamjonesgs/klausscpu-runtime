# Packages of our own go here (none yet).
include $(sort $(wildcard $(BR2_EXTERNAL_KLAUSSCPU_PATH)/package/*/*.mk))

# The stack grows down on KlaussCPU.  gnulib's c-stack/sigsegv (diffutils,
# gawk, grep, m4, ...) can't run a test program when cross-compiling and
# knows the direction only for the CPUs in its list: without these it
# leaves STACK_DIRECTION 0, and stackvma.c doesn't build.  Both cache
# variables are needed: given only the message, configure skips the
# check and defines STACK_DIRECTION as empty.
TARGET_CONFIGURE_OPTS += \
	sv_cv_stack_direction=-1 \
	sv_cv_stack_direction_msg="grows down" \
	ac_cv_c_stack_direction=-1


# git installs each built-in command as its own copy of the 3.9 MB git
# binary in libexec/git-core (171 of them, 550 MB): "git add" etc. don't
# need the dashed git-add, and the rest become symlinks.
GIT_MAKE_OPTS += SKIP_DASHED_BUILT_INS=YesPlease INSTALL_SYMLINKS=YesPlease

# Static linking: libcurl.a needs OpenSSL, zstd and zlib after it.  git's
# configure tests -lcurl alone (and so builds without HTTPS), and its
# Makefile links git-remote-http with `curl-config --libs` (just -lcurl).
GIT_CONF_ENV_LIBS += -lcurl -lssl -lcrypto -lzstd -lz -lpthread
GIT_MAKE_OPTS += CURL_LDFLAGS="-lcurl -lssl -lcrypto -lzstd -lz -lpthread"
