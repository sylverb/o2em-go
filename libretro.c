#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#pragma pack(1)
#endif

#include "libretro.h"
#include "libretro_core_options.h"

#include "audio.h"
#include "config.h"
#include "cpu.h"
#include "crc32.h"
#include "debug.h"
#include "keyboard.h"
#include "score.h"
#include "vdc.h"
#include "vmachine.h"
#include "voice.h"
#include "vpp.h"
#include "vkeyb/vkeyb.h"

#include "wrapalleg.h"

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;

static bool libretro_supports_bitmasks = false;

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_destroybmp(void);

char bios_file[16] = {0};

unsigned short int mbmp[TEX_WIDTH * TEX_HEIGHT];
uint8_t soundBuffer[SOUND_BUFFER_LEN];
static int16_t audioOutBuffer[SOUND_BUFFER_LEN * 2 * sizeof(int16_t)];
static int16_t audio_volume   = 50;
static bool low_pass_enabled  = false;
static int32_t low_pass_range = 0;
static int32_t low_pass_prev  = 0;
int SND;
int RLOOP=0;
int joystick_data[2][5]={{0,0,0,0,0},{0,0,0,0,0}};

int contax, o2flag, g74flag, c52flag, jopflag, helpflag;

unsigned long crcx = ~0;

static char scshot[MAXC],
odyssey2[MAXC],
file_v[MAXC],scorefile[MAXC], statefile[MAXC];

extern uint8_t intRAM[];

// True if the virtual keyboard must be showed
static bool vkb_show = false;

struct ButtonsState
{
  bool up, down, right, left;
  bool select, start;
  bool b, y;
};
// Last state of the buttons for joypad 1
struct ButtonsState last_btn_state = { false, false, false, false,
                                       false, false,
                                       false, false };

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   libretro_set_core_options(environ_cb);
}

static int does_file_exist(const char *filename)
{
   struct stat st;
   int result = stat(filename, &st);
   return result == 0;
}

static long filesize(FILE *stream)
{
   long curpos, length;
   curpos = ftell(stream);
   fseek(stream, 0L, SEEK_END);
   length = ftell(stream);
   fseek(stream, curpos, SEEK_SET);
   return length;
}

static bool load_bios(const char *biosname)
{
   FILE *fn;
   static char s[MAXC+10];
   unsigned long crc;
   int i;

   if ((biosname[strlen(biosname)-1]=='/') || (biosname[strlen(biosname)-1]=='\\') || (biosname[strlen(biosname)-1]==':'))
   {
      strcpy(s,biosname);
      strcat(s,odyssey2);
      fn = fopen(s,"rb");

      if (!fn)
      {
         strcpy(s,biosname);
         strcat(s,odyssey2);
         fn = fopen(s,"rb");
      }
   }
   else
   {
      strcpy(s,biosname);
      fn = fopen(biosname,"rb");
   }

   if (!fn)
   {
      fprintf(stderr,"Error loading bios ROM (%s)\n",s);
      return false;
   }

   if (fread(rom_table[0],1024,1,fn) != 1)
   {
      fclose(fn);
      fprintf(stderr,"Error loading bios ROM %s\n",odyssey2);
      return false;
   }

   fclose(fn);

   strcpy(s,biosname);
   fn = fopen(biosname,"rb");

   if (!fn)
   {
      fprintf(stderr,"Error loading bios ROM (%s)\n",s);
      return false;
   }

   if (fread(rom_table[0],1024,1,fn) != 1)
   {
      fclose(fn);
      fprintf(stderr,"Error loading bios ROM %s\n",odyssey2);
      return false;
   }

   fclose(fn);

   for (i=1; i<8; i++)
      memcpy(rom_table[i],rom_table[0],1024);

   crc = crc32_buf(rom_table[0],1024);

   if (crc==0x8016A315) {
      printf("Magnavox Odyssey2 BIOS ROM loaded (G7000 model)\n");
      app_data.vpp = 0;
      app_data.bios = ROM_O2;
   } else if (crc==0xE20A9F41) {
      printf("Philips Videopac+ European BIOS ROM loaded (G7400 model)\n");
      app_data.vpp = 1;
      app_data.bios = ROM_G7400;
   } else if (crc==0xA318E8D6) {
      if (!((!o2flag)&&(c52flag))) printf("Philips Videopac+ French BIOS ROM loaded (G7000 model)\n"); else printf("Ok\n");
      app_data.vpp = 0;
      app_data.bios = ROM_C52;
   } else if (crc==0x11647CA5) {
      if (g74flag) printf("Philips Videopac+ French BIOS ROM loaded (G7400 model)\n"); else printf(" Ok\n");
      app_data.vpp = 1;
      app_data.bios = ROM_JOPAC;
   } else {
      printf("Bios ROM loaded (unknown version)\n");
      app_data.vpp = 0;
      app_data.bios = ROM_UNKNOWN;
   }

   return true;
}

static bool load_cart(const char *file)
{
   FILE *fn;
   long l;
   int i, nb;

   app_data.crc = crc32_file(file);
   if (app_data.crc == 0xAFB23F89)
      app_data.exrom = 1;  /* Musician */
   if (app_data.crc == 0x3BFEF56B)
      app_data.exrom = 1;  /* Four in 1 Row! */
   if (app_data.crc == 0x9B5E9356)
      app_data.exrom = 1;  /* Four in 1 Row! (french) */

   if (((app_data.crc == 0x975AB8DA) || (app_data.crc == 0xE246A812)) && (!app_data.debug))
   {
      fprintf(stderr,"Error: file %s is an incomplete ROM dump\n",file);
      return false;
   }

   fn=fopen(file,"rb");
   if (!fn) {
      fprintf(stderr,"Error loading %s\n",file);
      return false;
   }
   printf("Loading: \"%s\"  Size: ",file);
   l = filesize(fn);

   if ((l % 1024) != 0) {
      fprintf(stderr,"Error: file %s is an invalid ROM dump\n",file);
      return false;
   }

   /* special MegaCART design by Soeren Gust */
   if ((l == 32768) || (l == 65536) || (l == 131072) || (l == 262144) || (l == 524288) || (l == 1048576))
   {
      app_data.megaxrom = 1;
      app_data.bank = 1;
      megarom = malloc(1048576);

      if (megarom == NULL)
      {
         fprintf(stderr, "Out of memory loading %s\n", file);
         return false;
      }
      if (fread(megarom, l, 1, fn) != 1)
      {
         fprintf(stderr,"Error loading %s\n",file);
         return false;
      }

      /* mirror shorter files into full megabyte */
      if (l < 65536)
         memcpy(megarom+32768,megarom,32768);
      if (l < 131072)
         memcpy(megarom+65536,megarom,65536);
      if (l < 262144)
         memcpy(megarom+131072,megarom,131072);
      if (l < 524288)
         memcpy(megarom+262144,megarom,262144);
      if (l < 1048576)
         memcpy(megarom+524288,megarom,524288);
      /* start in bank 0xff */
      memcpy(&rom_table[0][1024], megarom + 4096*255 + 1024, 3072);
      printf("MegaCart %ldK", l / 1024);
      nb = 1;
   }
   else if (((l % 3072) == 0))
   {
      app_data.three_k = 1;
      nb = l/3072;

      for (i=nb-1; i>=0; i--)
      {
         if (fread(&rom_table[i][1024],3072,1,fn) != 1)
         {
            fprintf(stderr,"Error loading %s\n",file);
            return false;
         }
      }
      printf("%dK",nb*3);
  
   } else {

      nb = l/2048;

      if ((nb == 2) && (app_data.exrom))
      {

         if (fread(&extROM[0], 1024,1,fn) != 1)
         {
            fprintf(stderr,"Error loading %s\n",file);
            return false;
         }
         if (fread(&rom_table[0][1024],3072,1,fn) != 1)
         {
            fprintf(stderr,"Error loading %s\n",file);
            return false;
         }
         printf("3K EXROM");

      }
      else
      {
         for (i=nb-1; i>=0; i--)
         {
            if (fread(&rom_table[i][1024],2048,1,fn) != 1)
            {
               fprintf(stderr,"Error loading %s\n",file);
               return false;
            }
            memcpy(&rom_table[i][3072],&rom_table[i][2048],1024); /* simulate missing A10 */
         }
         printf("%dK",nb*2);
      }
   }
   fclose(fn);
   rom = rom_table[0];
   if (nb==1)
      app_data.bank = 1;
   else if (nb==2)
      app_data.bank = app_data.exrom ? 1 : 2;
   else if (nb==4)
      app_data.bank = 3;
   else
      app_data.bank = 4;

   if ((rom_table[nb-1][1024+12]=='O') && (rom_table[nb-1][1024+13]=='P') && (rom_table[nb-1][1024+14]=='N') && (rom_table[nb-1][1024+15]=='B'))
      app_data.openb=1;

   printf("  CRC: %08lX\n",app_data.crc);

   return true;
}

void update_joy(void)
{
}

static void pointerToScreenCoordinates(int *x, int *y)
{
  *x = (*x + 0x7FFF) * EMUWIDTH / 0xFFFF;
  *y = (*y + 0x7FFF) * EMUHEIGHT / 0xFFFF;
}

static void update_input_virtual_keyboard(unsigned joypad_bits)
{
  bool select = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)) >> RETRO_DEVICE_ID_JOYPAD_SELECT;
  bool start  = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_START)) >> RETRO_DEVICE_ID_JOYPAD_START;
  bool y      = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_Y)) >> RETRO_DEVICE_ID_JOYPAD_Y;
  bool up     = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) >> RETRO_DEVICE_ID_JOYPAD_UP;
  bool down   = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) >> RETRO_DEVICE_ID_JOYPAD_DOWN;
  bool left   = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) >> RETRO_DEVICE_ID_JOYPAD_LEFT;
  bool right  = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) >> RETRO_DEVICE_ID_JOYPAD_RIGHT;
  bool b      = (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_B)) >> RETRO_DEVICE_ID_JOYPAD_B;
  bool click  = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);

  // Show the virtual keyboard?
  if (select && !last_btn_state.select)
  {
    vkb_show = !vkb_show;
    // Release current key when virtual keyboard hidden
    if (!vkb_show)
    {
      key[vkb_get_current_key_scancode()] = false;
    }
  }

  if (vkb_show)
  {
    // Move keyboard
    if (y && !last_btn_state.y)
    {
      vkb_set_virtual_keyboard_position((vkb_get_virtual_keyboard_position() + 1) % 2);
    }
    // Direct click on the keyboard (touch screen)
    if (click)
    {
      int xpointer, ypointer;
      xpointer = input_state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
      ypointer = input_state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
      pointerToScreenCoordinates(&xpointer, &ypointer);
      if (vkb_move_at(xpointer, ypointer))
      {
        // A key was touched: act as if the "Action" button was pushed
        b = true;
      }
    }
    // Press key
    if ((b && !last_btn_state.b) || (!b && last_btn_state.b))
    {
      key[vkb_get_current_key_scancode()] = b;
    }
    if (!b)
    {
      // Move current key
      if (right && !last_btn_state.right)
      {
        vkb_move_key(VKB_MOVE_RIGHT);
      }
      else if (left && !last_btn_state.left)
      {
        vkb_move_key(VKB_MOVE_LEFT);
      }
      else if (down && !last_btn_state.down)
      {
        vkb_move_key(VKB_MOVE_DOWN);
      }
      else if (up && !last_btn_state.up)
      {
        vkb_move_key(VKB_MOVE_UP);
      }
      // If start is pressed than press the Enter key
      if ((start && !last_btn_state.start) || (!start && last_btn_state.start))
      {
        key[RETROK_RETURN] = start;
      }
    }
  }

  last_btn_state.select = select;
  last_btn_state.start = start;
  last_btn_state.y = y;
  last_btn_state.b = b;
  last_btn_state.left = left;
  last_btn_state.right = right;
  last_btn_state.up = up;
  last_btn_state.down = down;
}

static void update_input(void)
{
   unsigned joypad_bits[2] = {0};
   size_t i, j;

   if (!input_poll_cb)
      return;

   input_poll_cb();

   for (i = 0; i < 2; i++)
   {
      if (libretro_supports_bitmasks)
         joypad_bits[i] = input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      else
         for (j = 0; j < (RETRO_DEVICE_ID_JOYPAD_R3+1); j++)
            joypad_bits[i] |= input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, j) ? (1 << j) : 0;

      /* If virtual KBD is shown, only need input
       * from port 1 */
      if (vkb_show)
         break;
   }

   if (!vkb_show)
   {
     // Joystick
     // Player 1
     joystick_data[0][0]= (joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) >> RETRO_DEVICE_ID_JOYPAD_UP;
     joystick_data[0][1]= (joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) >> RETRO_DEVICE_ID_JOYPAD_DOWN;
     joystick_data[0][2]= (joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) >> RETRO_DEVICE_ID_JOYPAD_LEFT;
     joystick_data[0][3]= (joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) >> RETRO_DEVICE_ID_JOYPAD_RIGHT;
     joystick_data[0][4]= (joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_B)) >> RETRO_DEVICE_ID_JOYPAD_B; /* "Action" button on the joystick */
     // Player 2
     joystick_data[1][0]= (joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) >> RETRO_DEVICE_ID_JOYPAD_UP;
     joystick_data[1][1]= (joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) >> RETRO_DEVICE_ID_JOYPAD_DOWN;
     joystick_data[1][2]= (joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) >> RETRO_DEVICE_ID_JOYPAD_LEFT;
     joystick_data[1][3]= (joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) >> RETRO_DEVICE_ID_JOYPAD_RIGHT;
     joystick_data[1][4]= (joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_B)) >> RETRO_DEVICE_ID_JOYPAD_B; /* "Action" button on the joystick */

     // Numeric and Alpha
     key[RETROK_0] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_0) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_X)) >> RETRO_DEVICE_ID_JOYPAD_X);
     key[RETROK_1] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_1) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_L)) >> RETRO_DEVICE_ID_JOYPAD_L);
     key[RETROK_2] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_2) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_R)) >> RETRO_DEVICE_ID_JOYPAD_R);
     key[RETROK_3] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_3) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_L2)) >> RETRO_DEVICE_ID_JOYPAD_L2);
     key[RETROK_4] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_4) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_R2)) >> RETRO_DEVICE_ID_JOYPAD_R2);
     key[RETROK_5] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_5) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_L3)) >> RETRO_DEVICE_ID_JOYPAD_L3);
     key[RETROK_6] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_6) |
         ((joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_R3)) >> RETRO_DEVICE_ID_JOYPAD_R3);

     key[RETROK_7] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_7);
     key[RETROK_8] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_8);
     key[RETROK_9] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_9);
     key[RETROK_a] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_a);
     key[RETROK_b] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_b);
     key[RETROK_c] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_c);
     key[RETROK_d] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_d);
     key[RETROK_e] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_e);
     key[RETROK_f] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_f);
     key[RETROK_g] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_g);
     key[RETROK_h] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_h);
     key[RETROK_i] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_i);
     key[RETROK_j] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_j);
     key[RETROK_k] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_k);
     key[RETROK_l] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_l);
     key[RETROK_m] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_m);
     key[RETROK_n] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_n);
     key[RETROK_o] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_o);
     key[RETROK_p] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_p);
     key[RETROK_q] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_q);
     key[RETROK_r] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_r);
     key[RETROK_s] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_s);
     key[RETROK_t] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_t);
     key[RETROK_u] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_u);
     key[RETROK_v] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_v);
     key[RETROK_w] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_w);
     key[RETROK_x] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_x);
     key[RETROK_y] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_y);
     key[RETROK_z] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_z);
     key[RETROK_SPACE] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_SPACE);       // Space
     key[RETROK_QUESTION] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_QUESTION); // ?
     key[RETROK_PERIOD] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PERIOD);     // .
     key[RETROK_END] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_END);           // "Clear"
     key[RETROK_RETURN] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RETURN);     // "Enter"
     key[RETROK_MINUS] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_MINUS);       // -
     key[RETROK_ASTERISK] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_ASTERISK); // Multiply sign
     key[RETROK_SLASH] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_SLASH);       // Divide sign
     key[RETROK_EQUALS] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_EQUALS);     // =
     key[RETROK_PLUS] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PLUS);         // +
   }

   // Virtual keyboard management
   update_input_virtual_keyboard(joypad_bits[0]);
}

static void upate_audio(void)
{
   uint8_t *audio_samples_ptr = soundBuffer;
   int16_t *audio_out_ptr     = audioOutBuffer;
   int length                 = (evblclk == EVBLCLK_NTSC) ? 44100 / 60 : 44100 / 50;

   /* Convert 8u mono to 16s stereo */
   if (low_pass_enabled)
   {
      int samples = length;

      /* Restore previous sample */
      int32_t low_pass = low_pass_prev;

      /* Single-pole low-pass filter (6 dB/octave) */
      int32_t factor_a = low_pass_range;
      int32_t factor_b = 0x10000 - factor_a;

      do
      {
         /* Get current sample */
         int16_t sample16 = (((*(audio_samples_ptr++) - 128) << 8) * audio_volume) / 100;

         /* Apply low-pass filter */
         low_pass = (low_pass * factor_a) + (sample16 * factor_b);

         /* 16.16 fixed point */
         low_pass >>= 16;

         /* Update output buffer */
         *(audio_out_ptr++) = (int16_t)low_pass;
         *(audio_out_ptr++) = (int16_t)low_pass;
      }
      while (--samples);

      /* Save last sample for next frame */
      low_pass_prev = low_pass;
   }
   else
   {
      int i;

      for(i = 0; i < length; i++)
      {
         int16_t sample16   = (((*(audio_samples_ptr++) - 128) << 8) * audio_volume) / 100;
         *(audio_out_ptr++) = sample16;
         *(audio_out_ptr++) = sample16;
      }
   }

   audio_batch_cb(audioOutBuffer, length);
}

/************************************
 * libretro implementation
 ************************************/

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
	info->library_name = "O2EM";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = "1.18" GIT_VERSION;
	info->need_fullpath = true;
	info->valid_extensions = "bin";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));

   info->timing.fps            = (evblclk == EVBLCLK_NTSC) ? 60 : 50;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = EMUWIDTH;
   info->geometry.base_height  = EMUHEIGHT;
   info->geometry.max_width    = EMUWIDTH;
   info->geometry.max_height   = EMUHEIGHT;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

size_t retro_serialize_size(void) 
{ 
	return savestate_size();
}

bool retro_serialize(void *data, size_t size)
{
   return savestate_to_mem((uint8_t *)data, size);
}

bool retro_unserialize(const void *data, size_t size)
{
   return loadstate_from_mem((uint8_t *)data, size);
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

bool retro_load_game(const struct retro_game_info *info)
{
    char bios_file_path[256];
    const char *full_path, *system_directory_c;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    struct retro_input_descriptor desc[] = {
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Action" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Move Virtual Keyboard" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Show/Hide Virtual Keyboard" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Numeric Key 0" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Numeric Key 1" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Numeric Key 2" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Numeric Key 3" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Numeric Key 4" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Numeric Key 5" },
       { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Numeric Key 6" },

       { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
       { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
       { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
       { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
       { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Action" },

       { 2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X, "Virtual Keyboard: Pointer X" },
       { 2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y, "Virtual Keyboard: Pointer Y" },
       { 2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED, "Virtual Keyboard: Pointer Pressed" },

       { 0 }
    };

    if (!info)
       return false;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "[O2EM]: RGB565 is not supported.\n");
        return false;
    }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

    full_path = info->path;
    system_directory_c = NULL;

    // BIOS is required
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);
    if (!system_directory_c)
    {
       if (log_cb)
          log_cb(RETRO_LOG_WARN, "[O2EM]: no system directory defined, unable to look for %s\n", bios_file);
       return false;
    }
    else
    {
#ifdef _WIN32
      char slash = '\\';
#else
      char slash = '/';
#endif

       snprintf(bios_file_path, sizeof(bios_file_path), "%s%c%s", system_directory_c, slash, bios_file);

       if (!does_file_exist(bios_file_path))
       {
          if (log_cb)
             log_cb(RETRO_LOG_WARN, "[O2EM]: %s not found, cannot load BIOS\n", bios_file);
          return false;
       }
    }

    app_data.debug = 0;
    app_data.stick[0] = app_data.stick[1] = 1;
    app_data.sticknumber[0] = app_data.sticknumber[1] = 0;
    set_defjoykeys(0,0);
    set_defjoykeys(1,1);
    set_defsystemkeys();
    app_data.bank = 0;
    app_data.limit = 1;
    app_data.sound_en = 1;
    app_data.speed = 100;
    app_data.wsize = 2;
    app_data.fullscreen = 0;
    app_data.scanlines = 0;
    app_data.voice = 1;
    app_data.window_title = "O2EM v" O2EM_VERSION;
    /* These volume settings have no effect
     * (they are allegro-specific) */
    app_data.svolume = 100;
    app_data.vvolume = 100;
    /* Internal audio filter is worthless,
     * disable it and use our own */
    app_data.filter = 0;
    app_data.exrom = 0;
    app_data.three_k = 0;
    app_data.crc = 0;
    app_data.scshot = scshot;
    app_data.statefile = statefile;
    app_data.openb = 0;
    app_data.vpp = 0;
    app_data.bios = 0;
    app_data.scoretype = 0;
    app_data.scoreaddress = 0;
    app_data.default_highscore = 0;
    app_data.breakpoint = 65535;
    app_data.megaxrom = 0;
    strcpy(scorefile,"highscore.txt");
    //read_default_config();

    init_audio();

    app_data.crc = crc32_file(full_path);

    //suck_bios();
    o2flag = 1;

    crcx = app_data.crc;
    //suck_roms();

    if (!load_bios(bios_file_path))
       return false;

    if (!load_cart(full_path))
       return false;

    //if (app_data.voice) load_voice_samples(path2);

    init_display();

    init_cpu();

    init_system();

    set_score(app_data.scoretype, app_data.scoreaddress, app_data.default_highscore);

    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void) 
{
}

unsigned retro_get_region(void)
{
    return evblclk == EVBLCLK_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
    if ( id == RETRO_MEMORY_SYSTEM_RAM )
        return intRAM;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    if ( id == RETRO_MEMORY_SYSTEM_RAM )
        return 64;
    return 0;
}

static void check_variables(bool startup)
{
   struct retro_variable var;

   if (startup)
   {
      bool auto_region = true;

      /* Console Region */
      var.key       = "o2em_region";
      var.value     = NULL;
      app_data.euro = 0;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "NTSC"))
         {
            app_data.euro = 0;
            auto_region   = false;
         }
         else if (!strcmp(var.value, "PAL"))
         {
            app_data.euro = 1;
            auto_region   = false;
         }
      }

      /* Emulated Hardware */
      var.key   = "o2em_bios";
      var.value = NULL;
      strcpy(bios_file, "o2rom.bin");

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         /* Ugly, but we don't want to inadvertently
          * copy an invalid value */
         if (!strcmp(var.value, "c52.bin"))
         {
            strcpy(bios_file, "c52.bin");
            if (auto_region)
               app_data.euro = 1;
         }
         else if (!strcmp(var.value, "g7400.bin"))
         {
            strcpy(bios_file, "g7400.bin");
            if (auto_region)
               app_data.euro = 1;
         }
         else if (!strcmp(var.value, "jopac.bin"))
         {
            strcpy(bios_file, "jopac.bin");
            if (auto_region)
               app_data.euro = 1;
         }
      }
   }

   /* Virtual KBD Transparency */
   var.key = "o2em_vkbd_transparency";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int alpha = 255 - (255 * atoi(var.value) / 100);
      vkb_set_virtual_keyboard_transparency(alpha);
   }

   /* Audio Volume */
   var.key      = "o2em_audio_volume";
   var.value    = NULL;
   audio_volume = 50;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      audio_volume = atoi(var.value);
      audio_volume = (audio_volume > 100) ? 100 : audio_volume;
      audio_volume = (audio_volume < 0)   ? 0   : audio_volume;
   }

   /* > Since we are not using the internal audio
    *   filter, volume must be divided by a factor
    *   of two */
   audio_volume = audio_volume >> 1;

   /* Audio Filter */
   var.key          = "o2em_low_pass_filter";
   var.value        = NULL;
   low_pass_enabled = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      if (strcmp(var.value, "enabled") == 0)
         low_pass_enabled = true;

   /* Audio Filter Level */
   var.key        = "o2em_low_pass_range";
   var.value      = NULL;
   low_pass_range = (60 * 0x10000) / 100;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      low_pass_range = (strtol(var.value, NULL, 10) * 0x10000) / 100;
}

void retro_init(void)
{
   struct retro_log_callback log;
   unsigned level = 5;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   libretro_supports_bitmasks = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   memset(mbmp, 0, sizeof(mbmp));
   vkb_configure_virtual_keyboard(mbmp, EMUWIDTH, EMUHEIGHT, TEX_WIDTH);

   check_variables(true);

   vkb_show      = false;
   low_pass_prev = 0;
   RLOOP         = 1;
}

void retro_deinit(void)
{
   close_audio();
   close_voice();
   close_display();
   retro_destroybmp();
}

void retro_reset(void)
{
   init_cpu();
   init_roms();
   init_vpp();
   clearscr();
}

void retro_run(void)
{
   bool var_updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &var_updated) && var_updated)
     check_variables(false);

   update_input();

   cpu_exec();
   RLOOP=1;

   if (vkb_show)
     vkb_show_virtual_keyboard();

   video_cb(mbmp, EMUWIDTH, EMUHEIGHT, TEX_WIDTH << 1);

   upate_audio();
}
