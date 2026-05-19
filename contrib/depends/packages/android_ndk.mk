package=android_ndk
$(package)_version=27c
$(package)_download_path=https://dl.google.com/android/repository/
$(package)_file_name=android-ndk-r$($(package)_version)-linux.zip
$(package)_sha256_hash=59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc

# This build host is macOS. depends ships the *linux* NDK, whose linux-x86_64
# clang cannot run here. Use the locally installed NDK r27c instead and stage
# its darwin (x86_64, via Rosetta) toolchain. The download above is left in
# place but unused.
$(package)_local_ndk=$(HOME)/Library/Android/sdk/ndk/27.2.12479018

define $(package)_extract_cmds
  mkdir -p $($(1)_extract_dir)
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir) && \
  cp -R $($(package)_local_ndk)/toolchains/llvm/prebuilt/darwin-x86_64/. $($(package)_staging_prefix_dir)/
endef
