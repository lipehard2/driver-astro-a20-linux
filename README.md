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

### Install (DKMS)

Requires `dkms` and the headers of each installed kernel. DKMS rebuilds the module automatically on kernel
updates, and it loads on its own whenever the transmitter is plugged in in game mode.

```sh
sudo pacman -S dkms        # Arch / CachyOS; use your distribution's package manager otherwise
sudo ./dkms-install.sh
```

Then reboot, or unplug and replug the transmitter. To uninstall: `sudo ./dkms-uninstall.sh`.

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

### Instalação (DKMS)

Requer o `dkms` e os headers de cada kernel instalado. O DKMS recompila o módulo sozinho a cada atualização do
kernel, e ele é carregado automaticamente sempre que o transmissor é plugado no modo game.

```sh
sudo pacman -S dkms        # Arch / CachyOS; em outras distribuições use o gerenciador de pacotes delas
sudo ./dkms-install.sh
```

Depois reinicie, ou desplugue e replugue o transmissor. Para desinstalar: `sudo ./dkms-uninstall.sh`.

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
