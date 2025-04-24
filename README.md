# simple-mp4-player
A very simple mp4 player that uses <a href="https://github.com/lieff/minimp4">minipm4</a> for demuxer, <a href="https://github.com/cisco/openh264">openh264</a> as decoder and SDL2 as player.


To compile you'll need SDL2-devel:

_Fedora_: `sudo dnf install SDL2-devel` 

<br/>
Compile example command:

`g++ mp4player.cpp -o mp4player -I./include -L./lib -lopenh264 -lSDL2`

<br/>

Video sample by <a href="https://pixabay.com/users/felixmittermeier-4397258/">FelixMittermeier</a> from <a href="https://pixabay.com/videos/sunset-sky-clouds-abendstimmung-8451/">Pixabay</a>
