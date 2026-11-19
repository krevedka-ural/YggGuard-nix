{
  description = "YggGuard — VPN-клиент поверх Yggdrasil (Qt GUI + привилегированный helper)";

  # Положи этот файл в YggGuard/client/ (рядом с CMakeLists.txt) и сделай `git add flake.nix`.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Пиры Yggdrasil по умолчанию (подставляются в диалог нового профиля).
      # Ограничения самого YggGuard: от 1 до 16 штук, схемы только tcp://, tls://, quic://
      # (ws:// клиент отвергнет). IPv6-пиры убраны: у большинства домашних провайдеров РФ
      # нет IPv6, а недоступные пиры только шумят в журнале.
      defaultPeers = [
        "tcp://45.147.200.202:12402"
        "tls://45.147.200.202:443"
        "tcp://45.95.202.21:12403"
        "tls://45.95.202.21:443"
        "tcp://ygg-ru.lskd.pw:30041"
        "tls://ygg-ru.lskd.pw:30042"
        "quic://ygg-ru.lskd.pw:30043"
        "tcp://cm1.wwire.su:2230"
        "tls://cm1.wwire.su:2231"
        "quic://cm1.wwire.su:2232"
        "tcp://89.44.86.85:65535"
        "quic://89.44.86.85:65535"
        "tcp://94.156.181.85:65535"
        "quic://94.156.181.85:65535"
      ];

      mkPackage = pkgs: { peers ? defaultPeers }:
        let
          inherit (pkgs) lib;

          # Без экранирования скобок: std::regex в Nix не принимает \] в POSIX-режиме.
          # Главное здесь: в пире нет кавычек и обратной косой, иначе сломается подстановка в C++/bash.
          peerOk = p: builtins.match "(tcp|tls|quic)://[^[:space:]'\"\\\\]+:[0-9]+" p != null;
          peerLiterals = lib.concatMapStringsSep ", " (p: "\"${p}\"") peers;

          # root-helper: чистый Go без зависимостей
          helper = pkgs.buildGoModule {
            pname = "yggguard-helper";
            version = "0.2.0";
            src = lib.fileset.toSource {
              root = ./helper;
              fileset = ./helper;
            };
            vendorHash = null;
            env.CGO_ENABLED = "0";
            doCheck = false;
            # В хелпере nft/resolvectl ищутся по PATH, который выставляет pkexec.
            # На NixOS он не гарантирован — прошиваем абсолютные пути из /nix/store.
            postPatch = ''
              substituteInPlace main.go \
                --replace-fail 'exec.LookPath("nft")' 'exec.LookPath("${pkgs.nftables}/bin/nft")'
              substituteInPlace dns.go \
                --replace-fail 'exec.LookPath("resolvectl")' 'exec.LookPath("${pkgs.systemd}/bin/resolvectl")'
            '';
          };
        in
        assert lib.assertMsg (peers != [ ] && lib.length peers <= 16)
          "yggguard: нужно от 1 до 16 пиров, получено ${toString (lib.length peers)}";
        assert lib.assertMsg (lib.all peerOk peers)
          "yggguard: пир должен быть tcp://, tls:// или quic:// вида хост:порт (ws:// не поддерживается)";
        pkgs.stdenv.mkDerivation {
          pname = "yggguard";
          version = "0.2.0";

          # build/, dist/, vendor/ (сотни МБ) в сборку не попадают
          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./CMakeLists.txt
              ./src
              ./assets
              ./tests
            ];
          };

          nativeBuildInputs = [ pkgs.cmake pkgs.qt6.wrapQtAppsHook ];
          buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtwayland ];
          cmakeFlags = [ "-DBUILD_TESTING=OFF" ];

          # В AppImage helper и движки копировались в /tmp, потому что pkexec не умел
          # исполнять файлы из FUSE-монтирования. В Nix это не нужно: /nix/store
          # неизменяем и принадлежит root, поэтому запускаем helper прямо оттуда.
          # Заодно закрывается дыра, когда root исполнял бинарники из каталога
          # обычного пользователя в /tmp.
          # SHELL убираем из окружения: pkexec отказывает, если оболочка вызывающего
          # (например, из `nix develop` или ~/.nix-profile) не указана в /etc/shells.
          postPatch = ''
            substituteInPlace src/main.cpp \
              --replace-fail 'static const QString DefaultPeer = "tcp://ygg-msk-1.averyan.ru:8363";' \
                             'static const QString DefaultPeer = QStringList{${peerLiterals}}.join(QLatin1Char(0x0A));' \
              --replace-fail 'QFile::copy(bundled(name),to)' 'true' \
              --replace-fail 'env.remove("LD_PRELOAD");' 'env.remove("LD_PRELOAD"); env.remove("SHELL");' \
              --replace-fail 'helper.start("pkexec", {stage->path()+"/yggguard-helper",' \
                             'helper.start("pkexec", {QFileInfo(bundled("yggguard-helper")).canonicalFilePath(),'
          '';

          postInstall = ''
            # GUI ищет всё в <bindir>/../libexec; helper определяет движки по os.Executable().
            # Поэтому helper — настоящая копия (не симлинк), движки — симлинки.
            install -Dm755 ${helper}/bin/* $out/libexec/yggguard-helper
            ln -s ${pkgs.yggdrasil}/bin/yggdrasil $out/libexec/yggdrasil
            ln -s ${pkgs.sing-box}/bin/sing-box   $out/libexec/sing-box

            # postInstall выполняется в build/, поэтому берём ассеты из $src
            install -Dm644 $src/assets/DejaVuSans.ttf   $out/share/yggguard/DejaVuSans.ttf
            install -Dm644 $src/assets/yggguard.svg     $out/share/icons/hicolor/scalable/apps/yggguard.svg
            install -Dm644 $src/assets/yggguard.desktop $out/share/applications/yggguard.desktop
          '';

          meta = {
            description = "VPN-клиент поверх Yggdrasil (HTTP-прокси на VPS через sing-box TUN)";
            platforms = systems;
            mainProgram = "yggguard";
          };
        };
    in
    {
      packages = forAll (pkgs: rec {
        yggguard = mkPackage pkgs { };
        default = yggguard;
      });

      apps = forAll (pkgs: rec {
        yggguard = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.yggguard}/bin/yggguard";
        };
        default = yggguard;
      });

      overlays.default = final: _prev: { yggguard = mkPackage final { }; };

      # programs.yggguard.enable = true;
      nixosModules.default = { config, lib, pkgs, ... }: {
        options.programs.yggguard = {
          enable = lib.mkEnableOption "YggGuard (VPN поверх Yggdrasil)";
          peers = lib.mkOption {
            type = lib.types.listOf lib.types.str;
            default = defaultPeers;
            description = ''
              Пиры Yggdrasil, подставляемые по умолчанию в диалог нового профиля.
              От 1 до 16, только tcp://, tls://, quic://.
            '';
          };
        };
        config = lib.mkIf config.programs.yggguard.enable {
          environment.systemPackages = [ (mkPackage pkgs { inherit (config.programs.yggguard) peers; }) ];
          # pkexec должен быть setuid-обёрткой из /run/wrappers и должен быть polkit-агент в DE
          security.polkit.enable = true;
          boot.kernelModules = [ "tun" "nf_tables" ];
          # Строгий rp_filter NixOS-фаервола режет ответы на TUN-интерфейсе (как у любого VPN)
          networking.firewall.checkReversePath = lib.mkDefault "loose";
        };
      };

      formatter = forAll (pkgs: pkgs.nixfmt-rfc-style);

      checks = forAll (pkgs: {
        build = self.packages.${pkgs.stdenv.hostPlatform.system}.yggguard;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.yggguard ];
          packages = [ pkgs.go pkgs.gopls ];
        };
      });
    };
}
