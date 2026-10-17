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

> Status: work in progress. Works end to end (music, microphone, PipeWire). Still missing: microphone gain and
> mixer controls, suspend testing, PC mode without rebooting, and DKMS installation.

### Build and load

Requires headers for the running kernel. Kernels built with Clang (e.g. CachyOS) are detected automatically and
built with `LLVM=1`.

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
- The microphone level is low (about 12–15 dB below typical voice levels). Gain control is being worked on.

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

> Status: em desenvolvimento. Funciona de ponta a ponta (música, microfone, PipeWire). Ainda faltam: ganho do
> microfone e controles de mixer, testes de suspensão, modo PC sem reiniciar e instalação via DKMS.

### Compilar e carregar

Requer os headers do kernel em execução. Kernels compilados com Clang (ex.: CachyOS) são detectados
automaticamente e compilados com `LLVM=1`.

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
- O nível do microfone está baixo (cerca de 12 a 15 dB abaixo do normal para voz). O controle de ganho está em
  desenvolvimento.

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
