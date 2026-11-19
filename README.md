# YggGuard на NixOS

Flake лежит в `client/`. Собирает GUI (Qt6), root-хелпер (Go) и берёт `yggdrasil` и `sing-box` из nixpkgs.

## Быстрый запуск

```bash
nix run "github:ВАШ_ЛОГИН/YggGuard?dir=client"
```

Для подключения нужны `pkexec` (setuid-обёртка NixOS) и polkit-агент в сессии.

## Установка в систему

```nix
{
  inputs.yggguard.url = "github:ВАШ_ЛОГИН/YggGuard?dir=client";

  outputs = { nixpkgs, yggguard, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        yggguard.nixosModules.default
        { programs.yggguard.enable = true; }
      ];
    };
  };
}
```

Модуль включает `security.polkit`, загружает модули ядра `tun` и `nf_tables`, ставит пакет и выставляет
`networking.firewall.checkReversePath = "loose"` (через `mkDefault`): строгий rpfilter режет ответы на TUN-интерфейсе.

Пользователь должен быть в группе `wheel` (иначе polkit не примет пароль администратора).

## Пиры Yggdrasil

В диалог нового профиля подставляется список российских пиров из `flake.nix` (`defaultPeers`).
Свой список можно задать в конфиге:

```nix
programs.yggguard.peers = [
  "tcp://ygg-ru.lskd.pw:30041"
  "quic://cm1.wwire.su:2232"
];
```

Ограничения клиента: от 1 до 16 пиров, схемы только `tcp://`, `tls://`, `quic://` (`ws://` не принимается).
Значения подставляются в сборку, поэтому после изменения пакет пересобирается. Уже сохранённые профили
не меняются: пиры в них правятся в окне профиля.

Системный `services.yggdrasil` для работы YggGuard **не нужен**: у клиента свой узел Yggdrasil (`yggguard0`)
с собственным ключом из профиля. Запущенный одновременно системный узел создаёт второй интерфейс и
конфликтует с ним по маршрутам `200::/7`.

## Что отличается от AppImage

- Хелпер запускается прямо из `/nix/store` (root-владелец, неизменяемый), а не копируется в `/tmp`.
- Пути к `nft` и `resolvectl` прошиты из nixpkgs: PATH под `pkexec` на NixOS не гарантирован.
- `SHELL` убирается из окружения перед `pkexec`, иначе он ругается на оболочки из `nix develop` и `~/.nix-profile`.

## Если что-то пошло не так

- `nix log .#yggguard` для ошибок сборки.
- Остался `inet yggguard_client` после аварийного завершения: `sudo nft delete table inet yggguard_client`.
- Версия sing-box из nixpkgs может отличаться от 1.12.x, под которую написан конфиг. Хелпер сделает `sing-box check` и покажет ошибку в журнале.
