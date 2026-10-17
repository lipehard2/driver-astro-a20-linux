# Astro A20 — protocolo no modo Game (`9886:0042`)

Observado com `tools/gip_probe` em 2026-10-17. Transmissor: fw `0.0.6366.0`, hw `3.1.1.1`.

## USB
- Interface 0 (FF/47/D0): EP `0x81` interrupt IN e EP `0x01` interrupt OUT, 64 B, bInterval 4.
- Interface 1: alt 0 sem endpoints; alt 1 com EP `0x02` iso OUT (maxp 208) e EP `0x83` iso IN (maxp 44), 1 pacote/ms.
- Nenhuma autenticação (cmd `0x06`) é pedida.

## Cabeçalho GIP
`cmd, options, seq, varint(len)[, varint(chunk_offset)]`, com comprimento total arredondado para par
(o último byte do varint ganha o bit 7 e é seguido de `00`).
Options: `0x0f` id do cliente, `0x10` pede ACK, `0x20` interno/sistema, `0x40` início de chunk, `0x80` chunk.

## Handshake observado
```
dev  03 20 .. 04 80 00 00 00            status: conectado, sem info de bateria
dev  02 20 .. 1c <28 B>                 announce (repetido a cada 250 ms até receber identify)
host 04 20 01 00                        identify
dev  04 f0 .. (chunked, 201 B total)    descritor; host faz ACK do primeiro e do último chunk
host 05 20 .. 01 01                     power SLEEP
host 08 20 .. 03 02 05 10               audio format: in=0x05 (16k mono) out=0x10 (48k estéreo)
dev  08 20 .. 03 02 05 10               formato aceito
host 05 20 .. 01 00                     power ON
dev  08 20 .. 05 00 04 32 00 32         volume (subcmd 0x00): mute=04 gain=0x32 out=0x00 in=0x32
dev  08 20 .. 05 00 04 32 32 32         volume atualizado
host SET_INTERFACE(1, 1), então stream iso nos dois sentidos
```

## Descritor (identify)
- Formatos: `05 10`
- Classes: `Windows.Xbox.Input.Headset`, `AstroGaming.Xbox.Devices.AstroDevice`
- Interfaces GUID: `04e9868a-4557-5c46-874b-deef21fe2a66`, `a3d125bc-4ec2-9249-9dda-ef4f123ef5dc`, `56ff7697-fd9b-8145-ad45-b645bba526d6`
- Comandos de cliente: offset `0xa1` (ainda não decodificado)

## Áudio
- OUT (host → fone): por ms, `60 20 <seq> c0 81 00` + 192 B S16_LE estéreo 48 kHz = 198 B.
- IN (fone → host): `60 20 <seq> <len> ...` + `le16 flow_rate` + amostras S16_LE mono 16 kHz.
  Com o mic sem som foram vistos só 6 B: `60 20 01 02 c0 00` (flow_rate = 192, sem amostras).

## Microfone e autenticação
- Sem autenticação, o fone envia o mic como **zeros absolutos** (os pacotes de 38 B chegam normalmente).
- Basta o host **iniciar** o handshake de autenticação para o mic começar a mandar amostras reais:
  ```
  host 06 30 .. 3a  00 41 00 01 00 2c  01 01 00 28  <32 B random> <8 B 0> <8 B trailer>   Host Hello v1
  dev  06 20 .. 06  00 c1 00 01 00 00                                                     auth ACK
  host 06 30 .. 0e  00 42 00 02 00 54  <8 B trailer>                                      pede Client Hello (80 B)
  dev  06 f0 .. (chunked, 90 B)  00 c2 00 02 00 54  02 01 00 50  <32 B random> <48 B>    Client Hello v1
  ```
- O fone usa a autenticação **v1** (RSA). Ainda não foi testado se ele volta a mutar sem concluir o handshake.
- Capacidades: o fone aceita `01 04 05 06 08 60` e envia `01 02 03 04 06 08 60`.
- Comando de cliente proprietário: `0x10`, até 64 B (classe `AstroGaming.Xbox.Devices.AstroDevice`).

## Travamento do firmware (importante para o driver)
- Depois de ter feito streaming em alt 1, um `SET_INTERFACE(1, 0)` trava o EP0 do dongle. Os SET_INTERFACE seguintes
  expiram, e depois os endpoints de interrupção também. Só replugar resolve (`usb_reset_device` derruba o dongle de vez).
- O kernel manda esse alt 0 sozinho quando a interface é liberada (`usb_unbind_interface`): ao fechar o libusb, no
  `rmmod`/unbind do driver.
- Consequência para o driver: ativar alt 1 **uma única vez por conexão** e manter o stream rodando (silêncio quando ocioso).
- Um reboot quente do PC deixa o dongle limpo: depois do reboot ele se anuncia de novo e o driver fica pronto ~1 s depois
  de carregar (testado em 2026-10-17, CachyOS 7.2.9). Ainda não foi testado suspender e retomar.

## Ganho do microfone
- O fone reporta o volume com o subcomando `0x00`: `00 04 32 32 32` (mute=04, gain=0x32, out=0x32, in=0x32).
- Ele **ignora** comandos de volume do host, tanto no formato `0x03` (`03 04 64 32 64 00 00 00`, usado pelo xone) quanto no
  formato `0x00` (`00 04 32 32 64`). Não responde, e o nível do mic não muda.
- Com voz, o mic fica entre −31 e −45 dBFS RMS, com ruído de fundo em −65 dBFS. O driver aplica ganho digital
  (controle ALSA "Mic Capture Volume", 0 a +24 dB, padrão +12 dB).
- O ganho de hardware provavelmente está no comando proprietário `0x10` da Astro (não mapeado; exigiria capturar o
  tráfego de um Xbox ou do app da Astro).
