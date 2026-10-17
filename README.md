# Astro A20 Linux driver

[English](#english) · [Português](#português)

---

## English

Kernel driver for the **Astro A20 (Gen 2)** wireless headset with its USB transmitter in **game mode**
(`9886:0042`). That is the factory default mode, and Linux does not support it: the transmitter speaks the
**Xbox GIP protocol** instead of USB Audio, so no stock driver binds to it and the headset never shows up as a
sound device.

With the `astro_a20` module loaded, the headset shows up as a regular ALSA/PipeWire device:

- 48 kHz stereo output
- 16 kHz mono microphone

> Status: work in progress. Works end to end (music, microphone, PipeWire). Still missing: suspend testing.

### Install

The driver is installed with DKMS, which rebuilds it automatically on every kernel update. Once installed, it loads
on its own whenever the transmitter is plugged in in game mode.

**1. Install the dependencies**

| Distribution | Command |
|---|---|
| Ubuntu, Linux Mint, Pop!_OS | `sudo apt install git dkms build-essential linux-headers-$(uname -r)` |
| Debian | `sudo apt install git dkms build-essential linux-headers-amd64` |
| Fedora | `sudo dnf install git dkms gcc make kernel-devel-matched` |
| Arch, EndeavourOS | `sudo pacman -S git dkms linux-headers` |
| CachyOS | `sudo pacman -S git dkms linux-cachyos-headers` |
| Manjaro | `sudo pacman -S git dkms linuxXY-headers` (e.g. `linux612-headers`; `mhwd-kernel -li` shows your kernel) |

On Ubuntu and Mint, make sure the headers metapackage of your kernel series is installed too
(`linux-headers-generic`, or `linux-headers-generic-hwe-24.04` on an HWE kernel), so that headers keep arriving with
kernel updates. If you use a kernel other than the default one (e.g. `linux-lts`, `linux-zen`), install its
`-headers` package as well.

**2. Download and install the driver**

```sh
git clone https://github.com/lipehard2/driver-astro-a20-linux.git
cd driver-astro-a20-linux
sudo ./dkms-install.sh
```

**3. Plug the transmitter in** (or unplug and replug it) and check:

```sh
aplay -l | grep A20     # should list "Astro A20"
```

To uninstall: `sudo ./dkms-uninstall.sh`.

#### Secure Boot

With Secure Boot enabled (check with `mokutil --sb-state`), the kernel only loads signed modules, and loading fails
with `Key was rejected by service`. DKMS signs the module with a local key, which has to be enrolled once:

- **Ubuntu, Mint, Debian:** the `dkms` installation asks for a password and enrolls the key automatically.
- **Fedora and others:** run `sudo mokutil --import /var/lib/dkms/mok.pub` and choose a password.

Then reboot. A blue *MOK management* screen appears: choose **Enroll MOK**, **Continue**, **Yes**, type the password
and reboot.

#### Compatibility

| Tested | Kernels |
|---|---|
| Builds (Ubuntu 22.04 and 24.04, Debian 13, Fedora 44, CachyOS) | 5.15 to 7.2 |
| Runs with the headset (CachyOS) | 6.18 LTS and 7.2 |

### Build and load manually (development)

Kernels built with Clang (e.g. CachyOS) are detected automatically and built with `LLVM=1`.

```sh
make module
sudo insmod src/astro_a20.ko
```

After loading the module, **replug the transmitter** (see "Known limitations").
To remove it: `sudo rmmod astro_a20`.

### Known limitations

- The transmitter firmware only accepts enabling the audio stream **once per plug**. Reloading the module while
  the transmitter is plugged in requires replugging it.
- The microphone only sends audio after the host starts the Xbox authentication handshake. The driver does
  this on its own.
- The transmitter always powers up in game mode, and its mode button only switches to PC mode right away while a
  GIP host is active. Without this driver the button seems to do nothing until the PC reboots; with it, the button
  switches between game and PC mode in about a second, in both directions.
- The headset ignores host volume commands, so microphone gain is applied in software: the ALSA control
  **Mic Capture Volume** goes from 0 to +24 dB (default 0 dB, no gain), e.g. `amixer -c A20 set Mic 6`.

### Layout

| Path | Contents |
|---|---|
| `src/` | kernel module (`main.c` USB, `gip.c` protocol, `audio.c` streaming and ALSA) |
| `tools/gip_probe.c` | userspace prototype (libusb) used to map the protocol |
| `docs/protocol.md` | observed protocol: handshake, formats, authentication, firmware quirks (in Portuguese) |
| `udev/70-astro-a20.rules` | user access to the USB device, to run `gip_probe` without root |

### License

GPL-2.0-or-later. The [xone](https://github.com/dlundqvist/xone) project was the reference for the GIP
protocol.

---

## Português

Driver de kernel para o headset sem fio **Astro A20 (Gen 2)** com o transmissor USB no **modo game**
(`9886:0042`). Esse é o modo em que ele sai de fábrica, e o Linux não o suporta: o transmissor fala o protocolo
**GIP do Xbox** em vez de USB Audio, então nenhum driver nativo se vincula e o fone não aparece como dispositivo
de som.

Com o módulo `astro_a20` carregado, o fone aparece como um dispositivo ALSA/PipeWire normal:

- saída estéreo 48 kHz
- microfone mono 16 kHz

> Status: em desenvolvimento. Funciona de ponta a ponta (música, microfone, PipeWire). Ainda falta: testar a
> suspensão.

### Instalação

O driver é instalado com o DKMS, que o recompila sozinho a cada atualização do kernel. Depois de instalado, ele é
carregado automaticamente sempre que o transmissor é plugado no modo game.

**1. Instale as dependências**

| Distribuição | Comando |
|---|---|
| Ubuntu, Linux Mint, Pop!_OS | `sudo apt install git dkms build-essential linux-headers-$(uname -r)` |
| Debian | `sudo apt install git dkms build-essential linux-headers-amd64` |
| Fedora | `sudo dnf install git dkms gcc make kernel-devel-matched` |
| Arch, EndeavourOS | `sudo pacman -S git dkms linux-headers` |
| CachyOS | `sudo pacman -S git dkms linux-cachyos-headers` |
| Manjaro | `sudo pacman -S git dkms linuxXY-headers` (ex.: `linux612-headers`; `mhwd-kernel -li` mostra o seu kernel) |

No Ubuntu e no Mint, confira se o metapacote de headers da sua série de kernel também está instalado
(`linux-headers-generic`, ou `linux-headers-generic-hwe-24.04` num kernel HWE), para que os headers continuem
chegando com as atualizações do kernel. Se você usa outro kernel além do padrão (ex.: `linux-lts`, `linux-zen`),
instale também o pacote `-headers` dele.

**2. Baixe e instale o driver**

```sh
git clone https://github.com/lipehard2/driver-astro-a20-linux.git
cd driver-astro-a20-linux
sudo ./dkms-install.sh
```

**3. Plugue o transmissor** (ou desplugue e replugue) e confira:

```sh
aplay -l | grep A20     # deve listar "Astro A20"
```

Para desinstalar: `sudo ./dkms-uninstall.sh`.

#### Secure Boot

Com o Secure Boot ativado (confira com `mokutil --sb-state`), o kernel só carrega módulos assinados, e o
carregamento falha com `Key was rejected by service`. O DKMS assina o módulo com uma chave local, que precisa ser
registrada uma vez:

- **Ubuntu, Mint, Debian:** a instalação do `dkms` pede uma senha e registra a chave automaticamente.
- **Fedora e outras:** rode `sudo mokutil --import /var/lib/dkms/mok.pub` e escolha uma senha.

Depois reinicie. Vai aparecer uma tela azul de *MOK management*: escolha **Enroll MOK**, **Continue**, **Yes**,
digite a senha e reinicie.

#### Compatibilidade

| Testado | Kernels |
|---|---|
| Compilação (Ubuntu 22.04 e 24.04, Debian 13, Fedora 44, CachyOS) | 5.15 a 7.2 |
| Funcionamento com o fone (CachyOS) | 6.18 LTS e 7.2 |

### Compilar e carregar manualmente (desenvolvimento)

Kernels compilados com Clang (ex.: CachyOS) são detectados automaticamente e compilados com `LLVM=1`.

```sh
make module
sudo insmod src/astro_a20.ko
```

Depois de carregar o módulo, **replugue o transmissor** (veja "Limitações conhecidas").
Para remover: `sudo rmmod astro_a20`.

### Limitações conhecidas

- O firmware do transmissor só aceita ativar o stream de áudio **uma vez por conexão**. Recarregar o módulo com
  o transmissor plugado exige replugá-lo.
- O microfone só manda áudio depois que o host inicia a autenticação Xbox. O driver faz isso sozinho.
- O transmissor sempre liga no modo game, e o botão de modo só troca para o modo PC na hora quando há um host GIP
  ativo. Sem este driver o botão parece não fazer nada até o PC ser reiniciado; com ele, o botão alterna entre game e
  PC em cerca de um segundo, nos dois sentidos.
- O fone ignora comandos de volume do host, então o ganho do microfone é aplicado por software: o controle ALSA
  **Mic Capture Volume** vai de 0 a +24 dB (padrão 0 dB, sem ganho), ex.: `amixer -c A20 set Mic 6`.

### Estrutura

| Caminho | Conteúdo |
|---|---|
| `src/` | módulo de kernel (`main.c` USB, `gip.c` protocolo, `audio.c` streaming e ALSA) |
| `tools/gip_probe.c` | protótipo em userspace (libusb) usado para mapear o protocolo |
| `docs/protocol.md` | protocolo observado: handshake, formatos, autenticação, quirks do firmware |
| `udev/70-astro-a20.rules` | acesso do usuário ao dispositivo USB, para usar o `gip_probe` sem root |

### Licença

GPL-2.0-or-later. O mapeamento do protocolo GIP teve como referência o projeto
[xone](https://github.com/dlundqvist/xone).
