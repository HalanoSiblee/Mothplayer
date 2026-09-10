# Mothplayer
Fast tui music player inspired by cmus with extra beautiful features

# Features
blazingly fast tui music player\
supported container format
- mp3
- flac
- m4a
- ogg
- wav

keys mapped (hotkeys for everything) F1 to print all\
varispeed control

## Metadata support

- Cover image (sixel)
- Title
- Artist
- Album
- Codec
- Rate
- Bitrate
- Channels

# BUILD

`make -j$(nproc)`

## Dependency

- ncurses
- ffmpeg
- libsixel
- alsa

# Screenshots

![](./media/1.png)
![](./media/2.png)
![](./media/3.png)