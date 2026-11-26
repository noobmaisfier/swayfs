{
  description = "swayfs development environment";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

    systems.url = "github:nix-systems/default-linux";

    flake-utils = {
      url = "github:numtide/flake-utils";
      inputs.systems.follows = "systems";
    };
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

      in {
        packages = rec {
          swayfs-unwrapped-git = pkgs.swayfx-unwrapped.overrideAttrs(old: {
            pname = "swayfs";
            version = "git";
            src = pkgs.lib.cleanSource ./.;
            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              wayland-scanner
              scdoc
            ];
            buildInputs = with pkgs; [
              libGL
              wayland
              libxkbcommon
              pcre2
              json_c
              libevdev
              cairo
              libinput
              gdk-pixbuf
              librsvg
              wayland-protocols
              libdrm
              libxcb-wm
              wlroots_0_20
            ];
          });

          default = swayfs-unwrapped-git;
        };

        devShells.default = pkgs.mkShell {
          name = "swayfs-shell";
          inputsFrom = [
            self.packages.${system}.swayfs-unwrapped-git
          ];
          packages = [ pkgs.gdb ]; # add debugging packages
          hardeningDisable = [ "fortify" ];
        };

        formatter = pkgs.nixfmt-rfc-style;
      }
    );
}

