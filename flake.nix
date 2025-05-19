{
  description = "A free and open source game engine";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {
      inherit system;
    };

	deps = [
		pkgs.alsa-lib
		pkgs.clang-tools
		pkgs.dbus
		pkgs.fontconfig
		pkgs.freetype
		pkgs.gdb
		pkgs.glib
		#pkgs.graphite2
		#pkgs.harfbuzz
		#pkgs.libbrotli
		#pkgs.libenet
		#pkgs.libpng
		#pkgs.libzstd
		pkgs.libxkbcommon
		#pkgs.libGL
		#pkgs.icu
		pkgs.mold
		pkgs.pipewire
		pkgs.pkg-config
		pkgs.pulseaudio
		pkgs.scons
		pkgs.speechd
		#pkgs.theora
		pkgs.udev
		pkgs.xorg.libX11
		pkgs.xorg.libXcursor
		pkgs.xorg.libXfixes
		pkgs.xorg.libXi
		pkgs.xorg.libXinerama
		pkgs.xorg.libXext
		pkgs.xorg.libXrandr
		pkgs.xorg.libXrender
		pkgs.wayland
		pkgs.wayland-scanner
		pkgs.wayland-utils
	];

	rpath = pkgs.lib.makeLibraryPath deps;

  in {
    devShells.x86_64-linux.default = pkgs.mkShell {
		packages = deps;

		env = {
			LDFLAGS = "-Wl,--rpath=${rpath}";
			LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [
				pkgs.libGL
				pkgs.vulkan-loader
			];
		};
    };
  };
}
