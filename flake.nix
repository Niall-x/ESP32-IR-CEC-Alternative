{
  description = "ESP32 IR Remote — PC daemon, firmware toolchain, and NixOS module";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f (import nixpkgs { inherit system; }));
    in
    {
      # -----------------------------------------------------------------------
      # Development shell
      #
      # The toolchain only; the build is plain CMake and PlatformIO.
      #
      #   nix develop
      #   cmake -B daemon/build -S daemon && cmake --build daemon/build
      #   cd firmware && pio run
      # -----------------------------------------------------------------------
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            pkg-config
            gcc
            hidapi
            # The unsuffixed sdbus-cpp is still 1.x in nixpkgs; 2.x is required.
            sdbus-cpp_2
            # libudev, for the device-arrival watch.
            systemdLibs
            # The FHS-wrapped PlatformIO: the ESP32 toolchain it downloads does
            # not run on bare NixOS.
            platformio
            # Serial console for firmware debugging.
            picocom
          ];
        };
      });

      # -----------------------------------------------------------------------
      # Daemon package
      #
      # Uses the project's own CMakeLists, so it matches every other distro.
      # -----------------------------------------------------------------------
      packages = forAllSystems (pkgs: rec {
        default = esp32-ir-daemon;

        esp32-ir-daemon = pkgs.stdenv.mkDerivation {
          pname = "esp32-ir-daemon";
          version = nixpkgs.lib.fileContents ./VERSION;

          # Only daemon/ and VERSION, so CAD changes do not rebuild the daemon.
          src = nixpkgs.lib.fileset.toSource {
            root = ./.;
            fileset = nixpkgs.lib.fileset.unions [ ./daemon ./VERSION ];
          };
          sourceRoot = "source/daemon";

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          buildInputs = with pkgs; [ hidapi sdbus-cpp_2 systemdLibs ];

          # CMake's install rules put the udev rule where services.udev.packages
          # finds it. The unit file it also installs is unused here.

          meta = with pkgs.lib; {
            description = "Mirrors PC power state to a TV over IR, as a CEC alternative";
            homepage = "https://github.com/Niall-xox/ESP32-IR-CEC-Alternative";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "esp32-ir-daemon";
          };
        };
      });

      # -----------------------------------------------------------------------
      # NixOS module
      #
      # Declares the service account, udev rule and service.
      #
      #   imports = [ inputs.esp32-ir-remote.nixosModules.default ];
      #   services.esp32-ir-remote.enable = true;
      # -----------------------------------------------------------------------
      nixosModules.default = { config, lib, pkgs, ... }:
        let cfg = config.services.esp32-ir-remote;
        in {
          options.services.esp32-ir-remote = {
            enable = lib.mkEnableOption "ESP32 IR Remote power-sync daemon";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.esp32-ir-daemon;
              defaultText = lib.literalExpression "esp32-ir-daemon";
              description = "The daemon package to run.";
            };
          };

          config = lib.mkIf cfg.enable {
            users.groups.esp32ir = { };
            users.users.esp32ir = {
              isSystemUser = true;
              group = "esp32ir";
              description = "ESP32 IR Remote daemon";
            };

            services.udev.packages = [ cfg.package ];

            systemd.services.esp32-ir-remote = {
              description = "ESP32 IR Remote Daemon";
              # Mirrors esp32-ir-remote.service.in; keep the two in step.
              after = [ "dbus.socket" "systemd-logind.service" ];
              requires = [ "systemd-logind.service" ];
              wantedBy = [ "multi-user.target" ];

              serviceConfig = {
                Type = "simple";
                ExecStart = lib.getExe cfg.package;
                Restart = "on-failure";
                RestartSec = 5;

                User = "esp32ir";
                Group = "esp32ir";

                NoNewPrivileges = true;
                ProtectSystem = "strict";
                ProtectHome = true;
                PrivateTmp = true;
                ProtectKernelTunables = true;
                ProtectKernelModules = true;
                ProtectControlGroups = true;
                RestrictNamespaces = true;
                LockPersonality = true;
                MemoryDenyWriteExecute = true;
                SystemCallArchitectures = "native";
                # AF_NETLINK is needed by the udev watch; see the unit file.
                RestrictAddressFamilies = [ "AF_UNIX" "AF_NETLINK" ];
                DeviceAllow = [ "char-hidraw rw" "char-usb_device rw" ];
              };
            };
          };
        };
    };
}
