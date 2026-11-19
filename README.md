# YggGuard на NixOS

Этот флейк вышел быстрее, чем оригинальный YggGuard от [korbal028](https://github.com/korbal028). Но, тут уже есть полноценное подключение к VPS по сети Yggdrasil. Вероятнее, будет реализована более полноценная поддержка .nix-конфигов, как в wireguard или оригинального Yggdrasil.

## Установка в систему

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    yggguard = {
      url = "github:krevedka-ural/YggGuard-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, yggguard, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        yggguard.nixosModules.default
      ];
    };
  };
}
```

Добавить в свой основной `comfiguration.nix`:

```nix
programs.yggguard.enable = true;
```

Модуль включает `security.polkit`, загружает модули ядра `tun` и `nf_tables`, ставит пакет и выставляет
`networking.firewall.checkReversePath = "loose"` (через `mkDefault`): строгий rpfilter режет ответы на TUN-интерфейсе.

Пользователь должен быть в группе `wheel` (иначе polkit не примет пароль администратора).

## Пиры Yggdrasil

Список российских пиров нужно задать в конфиге, необходимо для VPS, работающей в сети Yggdrasil:

```nix
programs.yggguard.peers = [
  # чекай пиры игга, если из РФ: https://github.com/yggdrasil-network/public-peers/blob/master/europe/russia.md
];
```

Ограничения клиента: от 1 до 16 пиров, схемы только `tcp://`, `tls://`, `quic://` (`ws://` не принимается).
Значения подставляются в сборку, поэтому после изменения пакет пересобирается. Уже сохранённые профили
не меняются: пиры в них правятся в окне профиля.

Системный `services.yggdrasil` для работы YggGuard **не нужен**: у клиента свой узел Yggdrasil (`yggguard0`)
с собственным ключом из профиля.

## Если что-то пошло не так

- `nix log .#yggguard` для ошибок сборки.
- Остался `inet yggguard_client` после аварийного завершения: `sudo nft delete table inet yggguard_client`.
- Версия sing-box из nixpkgs может отличаться от 1.12.x, под которую написан конфиг. Хелпер сделает `sing-box check` и покажет ошибку в журнале.
