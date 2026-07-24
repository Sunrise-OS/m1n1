#!/usr/bin/env sh

export RUSTC_BOOTSTRAP=1
exec rustc "$@"
