Speedup firefox launch speed by Jesus <GranPC> on droidian telegram community

# Usage

##

Open terminal with all the enviornment so it cant be a ssh terminal, you can open `screen` in a terminal on the phone and then ssh in and do `screen -x` to attach to it

##

Locate firefox-bin, prorbably in /usr/lib/firefox-esr/firefox-bin

In the same folder as libfirefote.so run 
```bash
export LD_PRELOAD="$LD_PRELOAD:./libfiregote.so" /usr/lib/firefox-esr/firefox-bin
```

you should see something similar to

```bash
gtk_window_set_title(0x783cb5e060, "Mozilla Firefox")
gtk_window_set_title called with the initial title. Waiting for the launch request.
```

##

Next we need to open up another terminal, doesnt need enviroment so it can be a normal ssh, and launch the firefox-client bin that was compiled
```bash
./firefox
```

The first terminal with screen should now say something like
```bash
Launch request received. Proceeding with the app initialization. Startup token:
```

##

Then locate firefox's desktop file (probably /usr/share/applications/firefox-esr.desktop)

clone it to /usr/share/applications/firefox-esr-boosted.desktop and change the following:

get rid of all Name[...] entries and leave just the first Name=. change that to something else so you can tell them apart in the app drawer. "Firefast" is a good option (alphabetic sort keeps them close together)

then find the Exec= line and replace it with Exec=<path-to-your-new-firefox-binary>, like so:

Exec=/home/user/gtk_hacks/firefox/firefox

save that and you should see both firefox entries in the app drawer

##

Close out the firefox that is open and in the screen terminal run

```bash
while true; do /usr/lib/firefox-esr/firefox-bin; done
```

and then caching should now be setup so you can then open the firefast desktop. Firefox should then open up quicker that normal

##

# Warnings

While that while command is running you will not be able to use the normal firefox desktop file, if you want to you can stop the while command and it should go back to normal and firefox will then take a while to open like normal.

When you close firefox when using this you will be unable to reopen it for 2-3 seconds, if you do it will just show the firefox splash screen for a while and then exit without openning, you can then try openning it again and it should work