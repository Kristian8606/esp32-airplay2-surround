<p align="center">
  <a href="images/stream.png"><img src="images/stream.png" alt="Wireless surround 7.1 / 5.1 / 2.1 from an ESP32 streamer: USB, radio and AirPlay to wireless speakers" width="100%"></a>
</p>

# Gallery

**From an ESP32 AirPlay receiver to a wireless surround streamer.** One
Gateway, now also on the ESP32-P4, plays AirPlay, web radio and music from
a USB stick to up to eight wireless speakers, 7.1, 5.1 or stereo with a
subwoofer, every speaker its own channel.

Click an image to open it full size. Back to the [README](README.md).

*The picture above is an illustration. A display and an enclosure for the
Gateway are planned. Today the Gateway is a bare board, and everything is
controlled from its web page.*

## The system

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/airplay-surround-dark.png"><img src="images/airplay-surround-dark.png" alt="ESP32 AirPlay 2 Surround 7.1" width="100%"></a>
      <br><sub><b>7.1 around the listener:</b> one Gateway, one ESP32-S3 board in every speaker</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/airplay-surround-light.png"><img src="images/airplay-surround-light.png" alt="Surround setup around a TV" width="100%"></a>
      <br><sub><b>No cables between the speakers:</b> the Gateway feeds them all over Wi-Fi</sub>
    </td>
  </tr>
</table>

## The Gateway's speakers page

<table>
  <tr>
    <td align="center" colspan="2">
      <a href="images/speakers-page-airplay.png"><img src="images/speakers-page-airplay.png" alt="Speakers page while AirPlay plays: Now playing with the album artwork, format, controls, and the room map with the front pair playing" width="100%"></a>
      <br><sub><b>AirPlay plays:</b> the sender's artwork (ESP32-P4 Gateway), the format, play/pause, skip and volume that go to the Apple device, and the room map with the speakers that play</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <a href="images/speakers-page.png"><img src="images/speakers-page.png" alt="Speakers page in the Warm theme: an album from the USB stick playing with its cover, and the room map" width="100%"></a>
      <br><sub><b>Music from the USB stick:</b> cover, title and album, bit for bit to the front pair</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="images/speakers-page-tiles.png"><img src="images/speakers-page-tiles.png" alt="One tile per speaker: role, level, delay and live sync" width="100%"></a>
      <br><sub><b>One tile per speaker:</b> role, level, delay and live sync</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/player-library.png"><img src="images/player-library.png" alt="USB player Library: folders and FLAC files, Play folder and + to the queue" width="100%"></a>
      <br><sub><b>The USB player's Library:</b> the stick's folders and files, <b>Play folder</b>, <b>+</b> to the queue</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="images/player-bar.png"><img src="images/player-bar.png" alt="Violet theme: the player with a FLAC album, its format chips and the stream each speaker gets" width="100%"></a>
      <br><sub><b>Violet theme:</b> the player with its format chips (<i>FLAC 44.1/16 · bit for bit</i>) and the ALAC stream each speaker gets</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/speakers-page-room.png"><img src="images/speakers-page-room.png" alt="Violet theme: room map and speaker tiles with sync, I2S correction, buffer and TSF quality" width="100%"></a>
      <br><sub><b>Live status of every speaker:</b> what it plays, sync, I2S correction, buffer fill and clock quality</sub>
    </td>
  </tr>
</table>

## Room calibration and Room EQ

<table>
  <tr>
    <td align="center" colspan="2">
      <a href="images/speakers-page-calibration-5.png"><img src="images/speakers-page-calibration-5.png" alt="Room calibration: distance, delay and level of five speakers measured from the seat" width="100%"></a>
      <br><sub><b>Room calibration:</b> five speakers measured from the seat with the Gateway's microphone, distance, delay and level each</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="images/roomeq-page.png"><img src="images/roomeq-page.png" alt="Room EQ page: measured response of a 2-way speaker, its target and the expected result" width="100%"></a>
      <br><sub><b>Room EQ:</b> a 2-way speaker's response, its target and the filters for its woofer and tweeter</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/speakers-page-calibration.png"><img src="images/speakers-page-calibration.png" alt="Room calibration without a microphone: every speaker played its chirps, nothing measured" width="100%"></a>
      <br><sub><b>Without a microphone:</b> every speaker still plays its chirps, so you hear that each one works</sub>
    </td>
  </tr>
</table>

## Apple Home

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/homekit-gateway.jpg"><img src="images/homekit-gateway.jpg" alt="The Gateway in Apple Home with artwork, playback and volume controls" width="300"></a>
      <br><sub><b>The whole system as one speaker</b> in Apple Home</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/homekit-player.jpg"><img src="images/homekit-player.jpg" alt="Apple Home speaker view with artwork and playback controls" width="300"></a>
      <br><sub><b>Artwork and playback controls</b> from the Home app</sub>
    </td>
  </tr>
</table>

## Building a speaker

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/wiring-max98357a.png"><img src="images/wiring-max98357a.png" alt="ESP32-S3 + MAX98357A I2S amplifier + speaker" width="100%"></a>
      <br><sub><b>The simplest speaker:</b> ESP32-S3 + MAX98357A I2S amplifier + one driver</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/wiring-pcm5102a-2way.jpg"><img src="images/wiring-pcm5102a-2way.jpg" alt="ESP32-S3 + PCM5102A DAC + 2-channel amplifier, woofer and tweeter" width="100%"></a>
      <br><sub><b>An active 2-way speaker:</b> ESP32-S3 + PCM5102A DAC + 2-channel amplifier, bi-amp woofer and tweeter</sub>
    </td>
  </tr>
</table>

Wiring is in [docs/hardware.md](docs/hardware.md), building and flashing in
[docs/install.md](docs/install.md).
