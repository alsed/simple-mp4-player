# simple-mp4-player
A very simple mp4 player that uses minipm4 for demuxer, openh264 as decoder and SDL2 as player.

To compile you'll need SDL2-devel:
(Fedora) `sudo dnf install SDL2-devel`

Compile example command:
`g++ mp4player.cpp -o mp4player -I./include -L./lib -lopenh264 -lSDL2`

Video sample by <a href="https://pixabay.com/users/felixmittermeier-4397258/">FelixMittermeier</a> from <a href="https://pixabay.com/videos/sunset-sky-clouds-abendstimmung-8451/">Pixabay</a>
