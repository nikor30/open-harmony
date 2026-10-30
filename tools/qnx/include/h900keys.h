/* Harmony 900 hard keys: scancode (key event +20, bit 7 = release) -> name.
 * From the guided mapping (mods/h900-keymap) cross-checked with the
 * photographed key history of mods/h900-input v4, corrected with
 * mods/h900-hao (keys shown by name): OFF is 0x22, HELP is 0x12. Scancode
 * 0x23 came at HELP's position in both mapping runs and is not assigned
 * yet. */
#ifndef H900KEYS_H
#define H900KEYS_H

static const struct h900key { unsigned char scan; const char *name; } h900keys[] = {
	{0x22, "OFF"},
	{0x02, "SIDE LEFT"}, {0x0D, "SIDE RIGHT"}, {0x1B, "UNDER LEFT"}, {0x1C, "UNDER RIGHT"},
	{0x41, "ACTIVITIES"}, {0x12, "HELP"}, {0x1A, "MENU"}, {0x53, "ROCKER UP"},
	{0x10, "INFO"}, {0x21, "EXIT"}, {0x0E, "ROCKER DOWN"}, {0x32, "GUIDE"},
	{0x16, "RED"}, {0x17, "GREEN"}, {0x1F, "YELLOW"}, {0x27, "BLUE"},
	{0x0A, "VOL +"}, {0x18, "VOL -"}, {0x0B, "CH +"}, {0x19, "CH -"},
	{0x48, "UP"}, {0x4B, "LEFT"}, {0x4C, "OK"}, {0x4D, "RIGHT"}, {0x50, "DOWN"},
	{0x25, "MUTE"}, {0x26, "BACK"},
	{0x2E, "REWIND"}, {0x2F, "PLAY"}, {0x01, "FORWARD"}, {0x04, "REPLAY"},
	{0x20, "SKIP"}, {0x13, "RECORD"}, {0x14, "PAUSE"}, {0x1E, "STOP"},
	{0x2B, "1"}, {0x2D, "2"}, {0x2C, "3"}, {0x39, "4"}, {0x29, "5"}, {0x31, "6"},
	{0x11, "7"}, {0x35, "8"}, {0x33, "9"}, {0x28, "CLEAR"}, {0x30, "0"}, {0x08, "ENTER"},
};
#define H900_NKEYS ((int)(sizeof(h900keys) / sizeof(h900keys[0])))

static __attribute__((unused)) const char *h900_key_name(int scan)
{
	int i;

	for (i = 0; i < H900_NKEYS; i++)
		if (h900keys[i].scan == scan)
			return h900keys[i].name;
	return NULL;
}

#endif
