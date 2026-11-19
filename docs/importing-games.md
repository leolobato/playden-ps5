# Importing games

You can add Windows games you already have to Playden, without a Steam account. This guide tells you where to put a
game folder, how to add it, and how to give the game a recipe and its own PS5 title art from files in the folder.

## Put the game folder in place

Put each game in its own folder, in one of these places:

- **On a USB drive or an M.2 SSD:** `playden/import/<game folder>` at the root of the drive. The drive must be exFAT.
  The PS5 can't read NTFS, and FAT32 can't hold files over 4 GB.
- **On the console, over FTP:** `/data/playden-import/<game folder>` (the console drop folder).

The game folder holds the game as it is on a PC: its program (`.exe`) and its files. It can also hold a folder named
`playden` with files for Playden:

```
playden/import/
└── My Game/
    ├── MyGame.exe
    ├── data/
    │   └── ...
    └── playden/
        ├── recipe.json      the game's settings (optional)
        ├── icon.png         its icon on PS5 Home, best 512x512 (optional)
        └── background.png   its background on PS5 Home, best 3840x2160 (optional)
```

Each file in `playden/` is optional. Playden reads only these three names. The folder and its files must be real
files, not links: Playden never follows a link out of the game folder.

## Add the game

1. Open **Import games**: on the Startup screen, on Home when your library is empty, or in Settings › Games.
2. Choose **Look for games**. Playden lists the game folders it finds, with their size.
3. Pick a folder. When it holds one program besides installers and tools, Playden starts the game with it.
   Otherwise, pick the program that starts the game. Installers and tools are labelled as such. You can change the
   program later in Game settings.
4. Choose where the game files live:
   - **Play from the USB drive** (or the M.2 SSD, or the drop folder): nothing is copied. The drive must stay plugged
     in.
   - **Copy to storage** (**Move to storage** for the drop folder), then the storage it goes to. Playden copies the
     game there. The console copies about 11 MB/s, so a large game takes a while.
5. Playden sets up the game. When it is ready, choose **Play**: Playden quits, and the PS5 adds the game's title to
   Home. Start the game from there.

The `playden` folder goes with the game when Playden copies or moves it. See the README,
[Import Windows games](../README.md#import-windows-games), for the limits on imported games.

## A recipe in the game folder

A recipe gives the game its settings: the program, its arguments, the graphics and controller settings, and the other
rows of Game settings. With `playden/recipe.json` in the game folder, the game is set up the right way when it is
added, with no changes in Game settings.

**Make the recipe:**

- Set up the game on a console until it runs well. Then, in **Game settings › Advanced**, choose **Export as recipe**.
  Playden writes the file to `/data/playden/exported-recipes` on the console. You can also get it from web access:
  **Game files**, the game's page, **Download exported recipe**.
- Or use a recipe from [playden-ps5-recipes](https://github.com/leolobato/playden-ps5-recipes), or a recipe somebody
  sent you.

Put the file in the game folder as `playden/recipe.json`.

**When you add the game,** Playden reads the recipe and asks "Use the recipe in the game's folder?":

- **Use the recipe:** the game gets the recipe's settings. If the folder was added before and you changed its settings,
  the recipe replaces them. When the recipe names no program, the game keeps the program you picked when you added it.
- **Don't use it:** the game keeps Playden's settings.

You can change the settings later in Game settings. The recipe can be for another copy of the game, or for a game with
another name: Playden doesn't compare the game it names with this one.

**What Playden checks:** the same as for a recipe imported from web access (see
[Import one from web access](community-recipes.md#import-one-from-web-access)). The file must be at most 16 KB, pass
the [recipe rules](community-recipes.md#rules-for-community-recipes), and have no INI values. A recipe that needs an
experimental feature works only when that feature is on.

When Playden can't use the recipe, it adds the game without it and shows the notice "Recipe not used", with the
reason. The file stays in the game folder.

Playden reads the recipe only when the game is added. To use a changed recipe later, import it from web access, or
remove the game from the library and add it again.

## Title art in the game folder

Without art, an imported game's PS5 title has Playden's generic icon (or the program's own icon), and the PS5's default
background on Home. To give the title your own art, put PNG pictures in the game's `playden` folder:

| File | Where it shows | What Playden does with it |
|---|---|---|
| `playden/icon.png` | The game's tile on PS5 Home and in the Game Library | Takes the middle square of the picture and makes it 512x512. |
| `playden/background.png` | Behind the game on PS5 Home when its tile is selected | Scales the picture to fill 3840x2160 (16:9) and cuts off what doesn't fit, keeping the middle. |

**The pictures:**

- PNG only, 8-bit or 16-bit, with or without transparency. See-through parts become black in the background.
- Any size up to 3840x2160 pixels, and at most 32 MB. A picture with another shape is cropped, not stretched.
- For the best result, use a 512x512 icon and a 3840x2160 background: Playden then doesn't scale them. A smaller
  16:9 background, such as 1920x1080, is scaled up. Keep what matters near the middle of the picture.

**When Playden reads them:** once, when it first sets up the game's PS5 title. Put the pictures in place before you add
the game. Playden doesn't use pictures that you add or change after that. Playden keeps the art with the game, so a
runtime kit update, a Repair and a move to other storage keep it. Art you set later with another app, such as ArtSwap,
stays too.

**When Playden can't use a picture,** it adds the game anyway and shows the notice "Title art not used". The notice
names the file and the reason. The title gets Playden's art in place of that picture.

| Reason in the notice | What to do |
|---|---|
| is not a PNG picture | Save the picture as PNG. A JPEG with a `.png` name is not a PNG. |
| is a damaged PNG picture | Save the picture again. |
| is larger than 3840x2160 pixels | Make the picture smaller. |
| is larger than 32 MB | Make the picture smaller, or save it with more compression. |
| is not a file | Replace the link or folder with the picture itself. |

The game's logo on Home and its music are not set from the folder.

## Prepare a game folder for others

When you pass a game folder to other players, you can include the `playden` folder:

1. Get the game running well on your console, and choose **Export as recipe**.
2. Copy the exported file into the game folder as `playden/recipe.json`.
3. Add `playden/icon.png` and `playden/background.png`.

A player who adds the folder gets your settings (when they choose **Use the recipe**) and your art.
