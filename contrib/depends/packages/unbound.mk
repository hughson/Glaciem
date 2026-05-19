package=unbound
$(package)_version=1.22.0
$(package)_download_path=https://www.nlnetlabs.nl/downloads/$(package)/
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=c5dd1bdef5d5685b2cedb749158dd152c52d44f65529a34ac15cd88d4b1b3d43
$(package)_dependencies=openssl
$(package)_patches=no-expat.patch

define $(package)_set_vars
  $(package)_config_opts=--disable-shared --enable-static --without-pyunbound --prefix=$(host_prefix)
  $(package)_config_opts+=--with-libexpat=no --with-ssl=$(host_prefix) --with-libevent=no
  $(package)_config_opts+=--without-pythonmodule --disable-flto --with-pthreads --with-libunbound-only
  # Forward-slash root-key path: the Windows default (C:\...\root.key) gets
  # its \r mangled by configure's echo, corrupting the generated header.
  # This path is cosmetic anyway under --with-libunbound-only.
  $(package)_config_opts+=--with-rootkey-file=/var/lib/unbound/root.key
  $(package)_config_opts_w64=--enable-static-exe --sysconfdir=/etc --prefix=$(host_prefix) --target=$(host_prefix)
  $(package)_config_opts_x86_64_darwin=ac_cv_func_SHA384_Init=yes
  $(package)_build_opts_mingw32=LDFLAGS="$($(package)_ldflags) -lpthread"
  $(package)_cflags_mingw32+="-D_WIN32_WINNT=0x600"
  # GCC 15 promotes these legacy-C diagnostics to hard errors by default,
  # which makes the older mingw-w64 headers fail to compile and breaks
  # unbound's configure tests. Relax them back to warnings.
  $(package)_cflags_mingw32+=-Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=implicit-int
endef

# Remove blobs
define $(package)_preprocess_cmds
  patch -p1 < $($(package)_patch_dir)/no-expat.patch &&\
  rm configure~ doc/*.odp doc/*.pdf contrib/*.tar.gz contrib/*.tar.bz2 &&\
  rm -rf testdata dnscrypt/testdata
endef

define $(package)_config_cmds
  $($(package)_autoconf) ac_cv_func_getentropy=no AR_FLAGS=$($(package)_arflags)
endef

define $(package)_build_cmds
  $(MAKE) $($(package)_build_opts)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf share
endef
