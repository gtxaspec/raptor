# Sanitizer and language flags shared by build-asan.sh (which sources
# this file as shell) and tests/Makefile (which includes it as make).
#
# That dual use constrains the syntax: every value must be free of
# spaces and quotes, since `sh` would treat a second word as a command
# and `make` would keep the quotes as part of the value. Compose
# multi-flag strings in the consumer, not here.
#
# Why share at all: 24 of the sources compiled by tests/Makefile are
# also compiled by build-asan.sh. When the two disagreed, the unit
# tests ran without UndefinedBehaviorSanitizer over shared code that
# the daemons ran with it.

RAPTOR_SAN_ADDRESS=-fsanitize=address,undefined
RAPTOR_SAN_THREAD=-fsanitize=thread
RAPTOR_SAN_EXTRA=-fno-omit-frame-pointer
RAPTOR_STD=gnu11
