#include "types.h"
#include "maple_if.h"
#include "maple_helper.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include "hw/pvr/spg.h"
#include "hw/naomi/naomi_cart.h"
#include <math.h>
#include <time.h>

#include "deps/zlib/zlib.h"
#include "deps/xxhash/xxhash.h"

#define LOGJVS(...) DEBUG_LOG(JVS, __VA_ARGS__)

const char* maple_sega_controller_name = "Dreamcast Controller";
const char* maple_sega_twinstick_name = "Twin Stick";
const char* maple_sega_vmu_name = "Visual Memory";
const char* maple_ascii_stick_name = "ASCII STICK";
const char* maple_sega_kbd_name = "Emulated Dreamcast Keyboard";
const char* maple_sega_mouse_name = "Emulated Dreamcast Mouse";
const char* maple_sega_dreameye_name_1 = "Dreamcast Camera Flash  Devic";
const char* maple_sega_dreameye_name_2 = "Dreamcast Camera Flash LDevic";
const char* maple_sega_mic_name = "MicDevice for Dreameye";
const char* maple_sega_purupuru_name = "Puru Puru Pack";
const char* maple_sega_lightgun_name = "Dreamcast Gun";

const char* maple_sega_brand = "Produced By or Under License From SEGA ENTERPRISES,LTD.";

bool enable_naomi_15khz_dipswitch = false;

enum MapleFunctionID
{
	MFID_0_Input       = 0x01000000, //DC Controller, Lightgun buttons, arcade stick .. stuff like that
	MFID_1_Storage     = 0x02000000, //VMU , VMS
	MFID_2_LCD         = 0x04000000, //VMU
	MFID_3_Clock       = 0x08000000, //VMU
	MFID_4_Mic         = 0x10000000, //DC Mic (, dreameye too ?)
	MFID_5_ARGun       = 0x20000000, //Artificial Retina gun ? seems like this one was never developed or smth -- I only remember of lightguns
	MFID_6_Keyboard    = 0x40000000, //DC Keyboard
	MFID_7_LightGun    = 0x80000000, //DC Lightgun
	MFID_8_Vibration   = 0x00010000, //Puru Puru Puur~~~
	MFID_9_Mouse       = 0x00020000, //DC Mouse
	MFID_10_StorageExt = 0x00040000, //Storage ? probably never used
	MFID_11_Camera     = 0x00080000, //DreamEye
};

enum MapleDeviceCommand
{
	MDC_DeviceRequest   = 0x01, //7 words.Note : Initialises device
	MDC_AllStatusReq    = 0x02, //7 words + device dependent ( seems to be 8 words)
	MDC_DeviceReset     = 0x03, //0 words
	MDC_DeviceKill      = 0x04, //0 words
	MDC_DeviceStatus    = 0x05, //Same as MDC_DeviceRequest ?
	MDC_DeviceAllStatus = 0x06, //Same as MDC_AllStatusReq ?

	//Various Functions
	MDCF_GetCondition   = 0x09, //FT
	MDCF_GetMediaInfo   = 0x0A, //FT,PT,3 pad
	MDCF_BlockRead      = 0x0B, //FT,PT,Phase,Block #
	MDCF_BlockWrite     = 0x0C, //FT,PT,Phase,Block #,data ...
	MDCF_GetLastError   = 0x0D, //FT,PT,Phase,Block #
	MDCF_SetCondition   = 0x0E, //FT,data ...
	MDCF_MICControl     = 0x0F, //FT,MIC data ...
	MDCF_ARGunControl   = 0x10, //FT,AR-Gun data ...

	MDC_JVSUploadFirmware = 0x80, // JVS bridge firmware
	MDC_JVSGetId		 = 0x82,
	MDC_JVSCommand		 = 0x86, // JVS I/O
};

enum MapleDeviceRV
{
	MDRS_JVSNone		 = 0x00, // No reply, used for multiple JVS I/O boards

	MDRS_DeviceStatus    = 0x05, //28 words
	MDRS_DeviceStatusAll = 0x06, //28 words + device dependent data
	MDRS_DeviceReply     = 0x07, //0 words
	MDRS_DataTransfer    = 0x08, //FT,depends on the command

	MDRE_UnknownFunction = 0xFE, //0 words
	MDRE_UnknownCmd      = 0xFD, //0 words
	MDRE_TransmitAgain   = 0xFC, //0 words
	MDRE_FileError       = 0xFB, //1 word, bitfield
	MDRE_LCDError        = 0xFA, //1 word, bitfield
	MDRE_ARGunError      = 0xF9, //1 word, bitfield

	MDRS_JVSReply		 = 0x87, // JVS I/O
};



#define SWAP32(a) ((((a) & 0xff) << 24)  | (((a) & 0xff00) << 8) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

//fill in the info
void maple_device::Setup(u32 prt)
{
	maple_port = prt;
	bus_port = maple_GetPort(prt);
	bus_id = maple_GetBusId(prt);
	logical_port[0] = 'A' + bus_id;
	logical_port[1] = bus_port == 5 ? 'x' : '1' + bus_port;
	logical_port[2] = 0;
}
maple_device::~maple_device()
{
	if (config)
		delete config;
}

/*
	Base class with dma helpers and stuff
*/
struct maple_base: maple_device
{
	u8* dma_buffer_out;
	u32* dma_count_out;

	u8* dma_buffer_in;
	u32 dma_count_in;

	void w8(u8 data) { *((u8*)dma_buffer_out)=data;dma_buffer_out+=1;dma_count_out[0]+=1; }
	void w16(u16 data) { *((u16*)dma_buffer_out)=data;dma_buffer_out+=2;dma_count_out[0]+=2; }
	void w32(u32 data) { *(u32*)dma_buffer_out=data;dma_buffer_out+=4;dma_count_out[0]+=4; }

	void wptr(const void* src,u32 len)
	{
		u8* src8=(u8*)src;
		while(len--)
			w8(*src8++);
	}
	void wstr(const char* str,u32 len)
	{
		size_t ln=strlen(str);
		verify(len>=ln);
		len-=ln;
		while(ln--)
			w8(*str++);

		while(len--)
			w8(0x20);
	}
	
	u8 r8()	  { u8  rv=*((u8*)dma_buffer_in);dma_buffer_in+=1;dma_count_in-=1; return rv; }
	u16 r16() { u16 rv=*((u16*)dma_buffer_in);dma_buffer_in+=2;dma_count_in-=2; return rv; }
	u32 r32() { u32 rv=*(u32*)dma_buffer_in;dma_buffer_in+=4;dma_count_in-=4; return rv; }
	void rptr(void* dst, u32 len)
	{
		u8* dst8=(u8*)dst;
		while(len--)
			*dst8++=r8();
	}
	u32 r_count() { return dma_count_in; }

	u32 Dma(u32 Command,u32* buffer_in,u32 buffer_in_len,u32* buffer_out,u32& buffer_out_len)
	{
		dma_buffer_out=(u8*)buffer_out;
		dma_count_out=&buffer_out_len;

		dma_buffer_in=(u8*)buffer_in;
		dma_count_in=buffer_in_len;

		return dma(Command);
	}
	virtual u32 dma(u32 cmd)=0;

	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out)
	{
		u32 command=buffer_in[0] &0xFF;
		//Recipient address
		u32 reci = (buffer_in[0] >> 8) & 0xFF;
		//Sender address
		u32 send = (buffer_in[0] >> 16) & 0xFF;
		u32 outlen = 0;
		u32 resp = Dma(command, &buffer_in[1], buffer_in_len - 4, &buffer_out[1], outlen);

		if (reci & 0x20)
			reci |= maple_GetAttachedDevices(maple_GetBusId(reci));

		verify(u8(outlen/4)*4==outlen);
		buffer_out[0] = (resp <<0 ) | (send << 8) | (reci << 16) | ((outlen / 4) << 24);

		return outlen + 4;
	}
};

/*
	Sega Dreamcast Controller
	No error checking of any kind, but works just fine
*/
struct maple_sega_controller: maple_base
{
	virtual u32 get_capabilities() {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfe060f00;	// 4 analog axes (0-3) X Y A B Start U D L R
	}

	virtual u32 transform_kcode(u32 kcode) {
		return kcode | 0xF901;		// mask off DPad2, C, D and Z;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs)
	{
		switch (index)
		{
		case 2:
			return pjs.joy[PJAI_X1];
		case 3:
			return pjs.joy[PJAI_Y1];
		case 0:
			return pjs.trigger[PJTI_R];		// Right trigger
		case 1:
			return pjs.trigger[PJTI_L];		// Left trigger
		default:
			return 0x80;					// unused
		}
	}

	virtual MapleDeviceType get_device_type()
	{
		return MDT_SegaController;
	}

	virtual const char *get_device_name()
	{
		return maple_sega_controller_name;
	}

	virtual const char *get_device_brand()
	{
		return maple_sega_brand;
	}

	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_controller::dma Called 0x%X;Command %d\n",device_instance->port,Command);
		switch (cmd)
		{
		case MDC_DeviceRequest:
			//caps
			//4
			w32(MFID_0_Input);

			//struct data
			//3*4
			w32(get_capabilities());
			w32( 0);
			w32( 0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);
			
			//30
			wstr(get_device_name(), 30);

			//60
			wstr(get_device_brand(), 60);

			//2
			w16(0x01AE);	// 43 mA

			//2
			w16(0x01F4);	// 50 mA

			return MDRS_DeviceStatus;

			//controller condition
		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				if (settings.System != DC_PLATFORM_NAOMI)
					config->GetInput(&pjs);

				//caps
				//4
				w32(MFID_0_Input);

				//state data
				//2 key code
				w16(transform_kcode(pjs.kcode));

				//triggers
				//1 R
				w8(get_analog_axis(0, pjs));
				//1 L
				w8(get_analog_axis(1, pjs));

				//joyx
				//1
				w8(get_analog_axis(2, pjs));
				//joyy
				//1
				w8(get_analog_axis(3, pjs));

				//not used on dreamcast
				//1
				w8(get_analog_axis(4, pjs));
				//1
				w8(get_analog_axis(5, pjs));
			}
			return MDRS_DataTransfer;

		default:
			//printf("UNKNOWN MAPLE COMMAND %d\n",cmd);
         return MDRE_UnknownCmd;
		}
	}
};

struct maple_atomiswave_controller : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff663f00;	// 6 analog axes, X Y L2/D2(?) A B C Start U D L R
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | AWAVE_TRIGGER_KEY;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		if (index < 2 || index > 5)
			return 0x80;
		index -= 2;
		u8 val;
		if (naomi_game_inputs != NULL && naomi_game_inputs->axes[index].name != NULL)
		{
			switch (naomi_game_inputs->axes[index].axis)
			{
			case 0:
				val = pjs.joy[0] - 0x80;
				break;
			case 1:
				val = pjs.joy[1] - 0x80;
				break;
			case 2:
				val = pjs.joy[2] - 0x80;
				break;
			case 3:
				val = pjs.joy[3] - 0x80;
				break;
			case 4:
				val = pjs.trigger[1];
				break;
			case 5:
				val = pjs.trigger[0];
				break;
			}
			if (naomi_game_inputs->axes[index].inverted)
				val = 0xff - val;
		}
		else
			val = pjs.joy[index] - 0x80;
		return val;
	}
};

/*
	Sega Twin Stick Controller
*/
struct maple_sega_twinstick : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfefe0000;	// no analog axes, X Y A B D Start U/D/L/R U2/D2/L2/R2
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | 0x0101;
	}

	virtual MapleDeviceType get_device_type() override {
		return MDT_TwinStick;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}
		
	virtual const char *get_device_name() override {
		return maple_sega_twinstick_name;
	}
};


/*
	Ascii Stick (Arcade Stick)
*/
struct maple_ascii_stick : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff070000;	// no analog axes, X Y Z A B C Start U/D/L/R
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | 0xF800;
	}

	virtual MapleDeviceType get_device_type() override {
		return MDT_AsciiStick;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}
		
	virtual const char *get_device_name() override {
		return maple_ascii_stick_name;
	}	
};

/*
	Sega Dreamcast Visual Memory Unit
	This is pretty much done (?)
*/


u8 vmu_default[] = {
		  0x78, 0x9c, 0xed, 0xdd, 0x5b, 0x88, 0x1c, 0x55, 0x1e, 0x07, 0xe0, 0xd3,
		  0x31, 0x6a, 0xf0, 0x3a, 0x0d, 0x0a, 0x5e, 0x18, 0x6a, 0x16, 0x02, 0x2e,
		  0xfb, 0x20, 0x68, 0x22, 0x2c, 0x28, 0x24, 0x21, 0x3d, 0xc2, 0x2e, 0xc6,
		  0x40, 0x26, 0xc3, 0x40, 0x2e, 0x90, 0x96, 0xec, 0xc8, 0x9a, 0xc4, 0x81,
		  0x6c, 0xa2, 0x20, 0x82, 0x11, 0xc1, 0x37, 0x41, 0x9f, 0x14, 0xf1, 0x61,
		  0x37, 0x64, 0xbc, 0x34, 0xb3, 0x17, 0xf0, 0xc1, 0x87, 0x0e, 0x9b, 0xb7,
		  0x55, 0x02, 0x3a, 0x19, 0x30, 0x8d, 0x0c, 0x1c, 0x82, 0xac, 0x17, 0x76,
		  0x59, 0x30, 0x77, 0xb5, 0x92, 0xce, 0xb1, 0xaa, 0x27, 0xc6, 0x98, 0x98,
		  0x68, 0xb2, 0x31, 0x15, 0x93, 0xef, 0x2b, 0x8a, 0x9a, 0xff, 0xef, 0xd4,
		  0xa9, 0x73, 0xe8, 0x3a, 0xf5, 0xd4, 0xc5, 0x74, 0x08, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd5, 0xfa, 0xf3, 0xdc, 0x2f, 0x47,
		  0x52, 0xfa, 0xae, 0xee, 0x7b, 0xfe, 0xf6, 0xaf, 0xe6, 0x0e, 0x84, 0xd0,
		  0x78, 0x20, 0x84, 0x4d, 0xa3, 0x95, 0x4d, 0x0b, 0x00, 0x7e, 0x31, 0xfe,
		  0xf8, 0xfb, 0x87, 0x6f, 0xa8, 0x7a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xc0, 0x95, 0xe1, 0x77, 0x9f, 0xfc, 0x69, 0xb2, 0x3c, 0xde, 0x7a, 0xed,
		  0x9c, 0xf0, 0xef, 0xa1, 0xe9, 0x63, 0x55, 0xcf, 0x07, 0x00, 0x00, 0x00,
		  0x00, 0x2e, 0x07, 0x0b, 0xcf, 0x92, 0x5d, 0x5f, 0x7b, 0x69, 0xf0, 0xc7,
		  0xce, 0xfd, 0x29, 0xd7, 0x2b, 0xc5, 0x3b, 0x47, 0x36, 0xbd, 0x58, 0xfb,
		  0xe7, 0x55, 0xc7, 0x8a, 0x6d, 0x7b, 0x2d, 0x84, 0x05, 0x0b, 0x6e, 0xfe,
		  0xed, 0x6d, 0x3f, 0x75, 0x92, 0x5c, 0x14, 0x0b, 0x65, 0x57, 0x5c, 0x56,
		  0xe5, 0xd8, 0xb2, 0x6a, 0xb3, 0x4b, 0x6d, 0x3e, 0xb2, 0x6a, 0xb2, 0x2a,
		  0xc7, 0x96, 0xb9, 0xef, 0xb2, 0xea, 0xb3, 0x2a, 0xc7, 0x96, 0xb9, 0xef,
		  0xb2, 0x8b, 0x9f, 0x5d, 0x6a, 0xf3, 0x91, 0x55, 0x93, 0x55, 0x39, 0xb6,
		  0xcc, 0x7d, 0x97, 0x55, 0x9f, 0x55, 0x39, 0xb6, 0xac, 0xda, 0xfb, 0xfe,
		  0x7f, 0x6a, 0x8c, 0x8d, 0x6d, 0x78, 0x64, 0x7d, 0x73, 0xed, 0x1f, 0x4e,
		  0x04, 0x1b, 0x9b, 0x8f, 0xac, 0xdf, 0xbc, 0x69, 0x6c, 0xe3, 0xb7, 0xc1,
		  0xd0, 0xba, 0xb1, 0x8d, 0x63, 0x8f, 0x37, 0x37, 0x9d, 0xe8, 0xb2, 0xbc,
		  0xa8, 0xd7, 0x7d, 0xef, 0x22, 0xbf, 0x2e, 0xf6, 0xeb, 0x8a, 0xbd, 0x16,
		  0x66, 0xfe, 0xd1, 0xd7, 0x96, 0x0d, 0xff, 0x69, 0xbc, 0xfc, 0xfc, 0xdb,
		  0xc5, 0x94, 0x9f, 0x5a, 0xfc, 0xdf, 0xa2, 0xe3, 0xb3, 0xb3, 0x26, 0xff,
		  0x75, 0xf2, 0xf9, 0x0b, 0x7f, 0x60, 0x22, 0xb2, 0xca, 0xd7, 0x02, 0xf0,
		  0x0b, 0xf3, 0x41, 0xd5, 0x13, 0xa0, 0x22, 0x7d, 0x67, 0x6a, 0x98, 0xf3,
		  0xa3, 0x5d, 0x6b, 0x17, 0x74, 0x22, 0xe7, 0xed, 0xb3, 0xbb, 0xa7, 0xef,
		  0x5f, 0x74, 0x7a, 0x9c, 0xae, 0x3f, 0x2d, 0xfa, 0xcd, 0x45, 0x98, 0x0d,
		  0x17, 0xdb, 0x19, 0x57, 0xf0, 0xec, 0x62, 0x9f, 0x75, 0xa2, 0x38, 0x96,
		  0xce, 0x55, 0xaf, 0xdf, 0xb9, 0x77, 0x3b, 0xa3, 0x2c, 0xec, 0x3d, 0xb7,
		  0x0e, 0x0b, 0xce, 0x7f, 0xf0, 0x67, 0x8a, 0xa9, 0x6f, 0x39, 0xfb, 0xe7,
		  0x76, 0xd3, 0x85, 0xf8, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xb8, 0x24, 0xa4, 0x6d, 0x31, 0xc6, 0x3d, 0xdd, 0x5d, 0x77, 0x64, 0xbb,
		  0xfa, 0xa7, 0xba, 0xf9, 0x93, 0x69, 0xe7, 0xea, 0x18, 0xf3, 0x38, 0x55,
		  0xef, 0xee, 0xea, 0xcf, 0x52, 0xeb, 0xe9, 0x94, 0x2f, 0x89, 0x71, 0xd5,
		  0x5f, 0x9b, 0x59, 0x77, 0xaa, 0x9e, 0x26, 0x27, 0x26, 0x77, 0x1c, 0xdd,
		  0xb6, 0x3a, 0x4e, 0x77, 0x9b, 0x59, 0x7c, 0x21, 0x4b, 0xf9, 0xe4, 0x91,
		  0x1d, 0x69, 0xe7, 0xb6, 0x46, 0xa3, 0xf1, 0xab, 0xb4, 0x7c, 0x38, 0x5b,
		  0x31, 0x91, 0x8e, 0xb4, 0x53, 0xda, 0xd9, 0x6a, 0xb5, 0x26, 0xf3, 0xd6,
		  0x44, 0x9a, 0x48, 0xa9, 0xdd, 0xde, 0x71, 0xf2, 0x97, 0xd2, 0x47, 0xda,
		  0xed, 0xf6, 0xf7, 0xcb, 0xd3, 0xdf, 0xdb, 0x98, 0xd3, 0x7b, 0x0f, 0xb4,
		  0x7c, 0xb5, 0x24, 0xbf, 0x2d, 0x84, 0xc7, 0x76, 0x7f, 0xfa, 0x79, 0x79,
		  0xac, 0xcd, 0x0a, 0x61, 0xd9, 0xa2, 0xc7, 0x9f, 0x68, 0x6e, 0x3c, 0xf9,
		  0xdc, 0x5b, 0x06, 0xeb, 0xb3, 0x9f, 0xdb, 0xfa, 0xca, 0xab, 0xff, 0xb8,
		  0x7a, 0xd1, 0xa3, 0xd7, 0x5c, 0x88, 0x8f, 0x63, 0xf6, 0x59, 0xda, 0x2e,
		  0xcb, 0xdf, 0xcc, 0x7b, 0x68, 0x69, 0x63, 0xe9, 0xd0, 0xd0, 0xc0, 0xc8,
		  0xa2, 0x65, 0x03, 0x3d, 0xcb, 0x06, 0x17, 0x2f, 0x5d, 0xd6, 0x18, 0x58,
		  0xfa, 0xe0, 0xc0, 0xa9, 0x2d, 0x33, 0x4e, 0xed, 0x3f, 0xf3, 0x0a, 0xd0,
		  0x6b, 0xb5, 0xb5, 0x4f, 0xfc, 0xf0, 0xf5, 0xbf, 0x4e, 0x07, 0xd3, 0x9b,
		  0xf9, 0xe8, 0xe1, 0xb5, 0x87, 0x87, 0x0e, 0xdd, 0x7b, 0xf0, 0xde, 0x03,
		  0xf7, 0x1c, 0x2c, 0xb6, 0x03, 0xd9, 0xde, 0x81, 0xfd, 0x7d, 0xfb, 0xc2,
		  0xbe, 0xb0, 0xb7, 0xdd, 0x7e, 0x67, 0xb8, 0xb0, 0xbc, 0xb1, 0x78, 0xde,
		  0xbc, 0xf9, 0x2b, 0xdb, 0xc7, 0xcb, 0xe1, 0xe1, 0xc1, 0x32, 0x29, 0x83,
		  0xcd, 0x2b, 0x8a, 0xe5, 0x55, 0x2e, 0xa9, 0x56, 0x6b, 0x7c, 0xde, 0x60,
		  0x7b, 0xfb, 0xf0, 0xbb, 0x1f, 0xd7, 0xeb, 0x33, 0xcb, 0x27, 0x1f, 0x9f,
		  0xbf, 0x72, 0xfb, 0xaa, 0xb4, 0xa1, 0x96, 0xba, 0x31, 0xee, 0x2e, 0x96,
		  0x71, 0x3e, 0x74, 0xff, 0xd1, 0x9d, 0xdd, 0x58, 0x2b, 0xca, 0x38, 0x51,
		  0xae, 0xeb, 0xf1, 0xfb, 0x52, 0x2b, 0x76, 0x7a, 0xf5, 0x92, 0x5e, 0x3d,
		  0x2f, 0xfd, 0x3d, 0x76, 0xea, 0x65, 0x3d, 0x5d, 0xec, 0x31, 0x9f, 0x9f,
		  0x26, 0xe2, 0x68, 0x56, 0xfe, 0xb9, 0xa7, 0x57, 0x37, 0xd2, 0x1b, 0x71,
		  0x4d, 0x51, 0x77, 0x46, 0xdf, 0x5f, 0x31, 0x53, 0x8f, 0xc7, 0x62, 0xad,
		  0x97, 0xe7, 0x7c, 0x78, 0xbc, 0xee, 0x34, 0xbb, 0xb1, 0xd3, 0x4c, 0x31,
		  0xae, 0xd8, 0x13, 0xf3, 0xc1, 0x34, 0x5e, 0x34, 0x75, 0x46, 0x6b, 0x33,
		  0x33, 0x28, 0xea, 0xad, 0xcd, 0x7a, 0xbd, 0x2f, 0xf4, 0x86, 0x98, 0x58,
		  0x9d, 0x8f, 0xa4, 0xad, 0x59, 0xfa, 0xa2, 0x5e, 0xcf, 0xca, 0xba, 0x68,
		  0x1f, 0x49, 0xaf, 0x17, 0x3d, 0x53, 0x3c, 0xae, 0xec, 0x5f, 0x1e, 0xa7,
		  0x5b, 0x6f, 0x95, 0x87, 0xd1, 0xff, 0x0d, 0x17, 0xf5, 0x9e, 0x38, 0x5a,
		  0x5c, 0xa3, 0x3b, 0x5a, 0x8c, 0x92, 0x95, 0xf5, 0xee, 0xce, 0x9a, 0x6c,
		  0x5f, 0x16, 0x9b, 0xb1, 0xd3, 0x97, 0xf5, 0xce, 0x7f, 0xbd, 0xd6, 0x9d,
		  0x2a, 0xea, 0x2f, 0x3a, 0xbb, 0x7a, 0xed, 0xf1, 0x70, 0xad, 0x78, 0x58,
		  0x3f, 0xaa, 0x67, 0x71, 0x2a, 0xf5, 0xea, 0xbf, 0xd5, 0xe3, 0x96, 0xfe,
		  0x4e, 0xbd, 0xbf, 0xd3, 0xcc, 0x7b, 0xf5, 0x87, 0xf5, 0xb8, 0xa6, 0xff,
		  0x2f, 0x7b, 0xfb, 0x3b, 0x59, 0x51, 0x17, 0xcf, 0x7b, 0x27, 0xef, 0x2f,
		  0x1e, 0xee, 0xe2, 0x79, 0x4f, 0xf9, 0xc8, 0xcf, 0xb9, 0xb6, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xe0, 0x72, 0x73, 0x34, 0xd9, 0x2e, 0xef, 0x2d, 0x4f, 0xfb, 0xc2, 0xfe,
		  0x70, 0x20, 0x1c, 0x0c, 0x87, 0xc2, 0xe1, 0xf0, 0x65, 0xf8, 0x2a, 0x7c,
		  0x1d, 0xf2, 0x70, 0x24, 0x1c, 0x0d, 0x79, 0xd1, 0x36, 0x7c, 0x8a, 0x5a,
		  0xf9, 0xc3, 0x7a, 0x67, 0x59, 0x2f, 0xb7, 0xbf, 0x52, 0xbf, 0x2b, 0x84,
		  0x81, 0x13, 0xbf, 0x93, 0x98, 0x7a, 0xfb, 0xb1, 0x50, 0x0b, 0xdd, 0x70,
		  0x63, 0xd8, 0x1c, 0xde, 0x0b, 0x59, 0x91, 0x6c, 0xf9, 0xb9, 0x97, 0x2d,
		  0x17, 0xc8, 0x37, 0x7a, 0x6b, 0xe6, 0x6e
};

struct maple_sega_vmu: maple_base
{
	FILE* file;
	u8 flash_data[128*1024];
	u8 lcd_data[192];
	u8 lcd_data_decoded[VMU_SCREEN_WIDTH*VMU_SCREEN_HEIGHT];
	
	virtual MapleDeviceType get_device_type()
	{
		return MDT_SegaVMU;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_SA(flash_data,128*1024);
		LIBRETRO_SA(lcd_data,192);
		LIBRETRO_SA(lcd_data_decoded,48*32);
		return true ;
	}
	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_USA(flash_data,128*1024);
		LIBRETRO_USA(lcd_data,192);
		LIBRETRO_USA(lcd_data_decoded,48*32);
		return true ;
	}
	virtual void OnSetup()
	{
		memset(flash_data,0,sizeof(flash_data));
		memset(lcd_data,0,sizeof(lcd_data));
		string apath = get_writable_vmu_path(logical_port);

		vmu_screen_params[bus_id].vmu_lcd_screen = lcd_data_decoded ;

		uLongf dec_sz = sizeof(flash_data);
		INFO_LOG(MAPLE, "Initializing VMU data...");
		int rv=uncompress(flash_data, &dec_sz, vmu_default, sizeof(vmu_default));

		verify(rv == Z_OK);
		verify(dec_sz == sizeof(flash_data));

		file=fopen(apath.c_str(),"rb+");
		if (!file)
		{
			INFO_LOG(MAPLE, "Unable to open VMU save file \"%s\", creating new file",apath.c_str());
			file=fopen(apath.c_str(),"wb");
			if (file) {
				fwrite(flash_data, sizeof(flash_data), 1, file);
				fseek(file,0,SEEK_SET);
			} else {
				INFO_LOG(MAPLE, "Unable to create VMU!");
			}
		}

		if (!file)
		{
			WARN_LOG(MAPLE, "Failed to create VMU save file \"%s\"", apath.c_str());
		}
		else
		{
			fread(flash_data,1,sizeof(flash_data),file);
			NOTICE_LOG(MAPLE, "Loaded VMU from file \"%s\"", apath.c_str());
		}
	}
	virtual ~maple_sega_vmu()
	{
		if (file) fclose(file);
	}
	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_vmu::dma Called for port 0x%X, Command %d\n",device_instance->port,Command);
		switch (cmd)
		{
		case MDC_DeviceRequest:
			{
				//caps
				//4
				w32(MFID_1_Storage | MFID_2_LCD | MFID_3_Clock);

				//struct data
				//3*4
				w32( 0x403f7e7e); // for clock
				w32( 0x00100500); // for LCD
				w32( 0x00410f00); // for storage
				//1  area code
				w8(0xFF);
				//1  direction
				w8(0);
				//30
				wstr(maple_sega_vmu_name,30);

				//60
				wstr(maple_sega_brand,60);

				//2
				w16(0x007c);

				//2
				w16(0x0082);

				return MDRS_DeviceStatus;
			}

			//in[0] is function used
			//out[0] is function used
		case MDCF_GetMediaInfo:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						w32(MFID_1_Storage);
						
						if (*(u16*)&flash_data[0xFF * 512 + 0x40] != 0xFF)
						{
							// Unformatted state: return predetermined media information
							//total_size;
							w16(0xff);
							//partition_number;
							w16(0);
							//system_area_block;
							w16(0xFF);
							//fat_area_block;
							w16(0xfe);
							//number_fat_areas_block;
							w16(1);
							//file_info_block;
							w16(0xfd);
							//number_info_blocks;
							w16(0xd);
							//volume_icon;
							w8(0);
							//reserved1;
							w8(0);
							//save_area_block;
							w16(0xc8);
							//number_of_save_blocks;
							w16(0x1f);
							//reserverd0 (something for execution files?)
							w32(0);
						}
						else
						{
							// Get data from the vmu system area (block 0xFF)
							wptr(flash_data + 0xFF * 512 + 0x40, 24);
						}

						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_2_LCD:
					{
						u32 pt=r32();
						if (pt!=0)
						{
							INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> bad input |%08X|, returning MDRE_UnknownCmd", pt);
							return MDRE_UnknownCmd;
						}
						else
						{
							w32(MFID_2_LCD);

							w8(47);             //X dots -1
							w8(31);             //Y dots -1
							w8(((1)<<4) | (0)); //1 Color, 0 contrast levels
							w8(2);              //Padding
							
							return MDRS_DataTransfer;
						}
					}
					break;

				default:
					INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> Bad function used |%08X|, returning -2", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockRead:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						w32(MFID_1_Storage);
						u32 xo=r32();
						u32 Block = (SWAP32(xo))&0xffff;
						w32(xo);

						if (Block>255)
						{
							DEBUG_LOG(MAPLE, "Block read : %d", Block);
							DEBUG_LOG(MAPLE, "BLOCK READ ERROR");
							Block&=255;
						}
						wptr(flash_data+Block*512,512);

						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_2_LCD:
					{
						w32(MFID_2_LCD);
						w32(r32()); // mnn ?
						wptr(flash_data,192);
						
						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_3_Clock:
					{
						if (r32()!=0)
						{
							INFO_LOG(MAPLE, "VMU: Block read: MFID_3_Clock : invalid params");
							return MDRE_TransmitAgain; //invalid params
						}
						else
						{
							w32(MFID_3_Clock);

							time_t now;
							time(&now);
							tm* timenow=localtime(&now);
							
							u8* timebuf=dma_buffer_out;

							w8((timenow->tm_year+1900)%256);
							w8((timenow->tm_year+1900)/256);

							w8(timenow->tm_mon+1);
							w8(timenow->tm_mday);

							w8(timenow->tm_hour);
							w8(timenow->tm_min);
							w8(timenow->tm_sec);
							w8(0);

							DEBUG_LOG(MAPLE, "VMU: CLOCK Read-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d!",
									timebuf[0] + timebuf[1] * 256,
									timebuf[2],
									timebuf[3],
									timebuf[4],
									timebuf[5],
									timebuf[6]);

							return MDRS_DataTransfer;//transfer reply ...
						}
					}
					break;

				default:
					INFO_LOG(MAPLE, "VMU: cmd MDCF_BlockRead -> Bad function |%08X| used, returning -2", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockWrite:
			{
				switch(r32())
				{
					case MFID_1_Storage:
					{
						u32 bph=r32();
						u32 Block = (SWAP32(bph))&0xffff;
						u32 Phase = ((SWAP32(bph))>>16)&0xff; 
						u32 write_adr=Block*512+Phase*(512/4);
						u32 write_len=r_count();
						if (write_adr + write_len > sizeof(flash_data))
							return MDRE_TransmitAgain; //invalid params
						rptr(&flash_data[write_adr],write_len);

						if (file)
						{
							fseek(file,write_adr,SEEK_SET);
							fwrite(&flash_data[write_adr],1,write_len,file);
							fflush(file);
						}
						else
						{
							INFO_LOG(MAPLE, "Failed to save VMU %s data", logical_port);
						}
						return MDRS_DeviceReply;//just ko
					}
					break;
					

					case MFID_2_LCD:
					{
						u32 wat=r32();
						rptr(lcd_data,192);
						
						u8 white=0xff,black=0x00;

						for(int y=0;y<32;++y)
						{
							u8* dst=lcd_data_decoded+y*48;
							u8* src=lcd_data+6*y+5;
							for(int x=0;x<6;++x)
							{
								u8 col=*src--;
								for(int l=0;l<8;l++)
								{
									*dst++=col&1?black:white;
									col>>=1;
								}
							}
						}
						config->SetImage(lcd_data_decoded);
						push_vmu_screen(lcd_data_decoded);
						return  MDRS_DeviceReply;//just ko
					}
					break;

					case MFID_3_Clock:
					{
						if (r32()!=0 || r_count()!=8)
							return MDRE_TransmitAgain;	//invalid params ...
						else
						{
							u8 timebuf[8];
							rptr(timebuf,8);
							DEBUG_LOG(MAPLE, "VMU: CLOCK Write-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d! Nothing set tho ...",
									timebuf[0]+timebuf[1]*256,timebuf[2],timebuf[3],timebuf[4],timebuf[5],timebuf[6]);
							return  MDRS_DeviceReply;//ok !
						}
					}
					break;

					default:
					{
						INFO_LOG(MAPLE, "VMU: command MDCF_BlockWrite -> Bad function used, returning MDRE_UnknownFunction");
						return  MDRE_UnknownFunction;//bad function
					}
				}
			}
			break;

		case MDCF_GetLastError:
			return MDRS_DeviceReply;//just ko

		case MDCF_SetCondition:
			{
				switch(r32())
				{
				case MFID_3_Clock:
					{
						u32 bp=r32();
						if (bp)
						{
							INFO_LOG(MAPLE, "BEEP : %08X", bp);
						}
						return  MDRS_DeviceReply;//just ko
					}
					break;

				default:
					{
						INFO_LOG(MAPLE, "VMU: command MDCF_SetCondition -> Bad function used, returning MDRE_UnknownFunction");
						return MDRE_UnknownFunction;//bad function
					}
					break;
				}
			}


		default:
			DEBUG_LOG(MAPLE, "Unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}	
};


struct maple_microphone: maple_base
{
	u8 micdata[SIZE_OF_MIC_DATA];

	virtual MapleDeviceType get_device_type()
	{
		return MDT_Microphone;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_SA(micdata,SIZE_OF_MIC_DATA);
		return true ;
	}
	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_USA(micdata,SIZE_OF_MIC_DATA);
		return true ;
	}
	virtual void OnSetup()
	{
		memset(micdata,0,sizeof(micdata));
	}

	virtual u32 dma(u32 cmd)
	{
		//printf("maple_microphone::dma Called 0x%X;Command %d\n",this->maple_port,cmd);
		//LOGD("maple_microphone::dma Called 0x%X;Command %d\n",this->maple_port,cmd);

		switch (cmd)
		{
		case MDC_DeviceRequest:
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceRequest");
			//this was copied from the controller case with just the id and name replaced!

			//caps
			//4
			w32(MFID_4_Mic);

			//struct data
			//3*4
			w32( 0xfe060f00);
			w32( 0);
			w32( 0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);
			
			//30
			wstr(maple_sega_mic_name,30);

			//60
			wstr(maple_sega_brand,60);

			//2
			w16(0x01AE); 

			//2
			w16(0x01F4);

			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			{
				DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_GetCondition");
				//this was copied from the controller case with just the id replaced!

				//PlainJoystickState pjs;
				//config->GetInput(&pjs);
				//caps
				//4
				w32(MFID_4_Mic);

				//state data
				//2 key code
				//w16(pjs.kcode);

				//triggers
				//1 R
				//w8(pjs.trigger[PJTI_R]);
				//1 L
				//w8(pjs.trigger[PJTI_L]);

				//joyx
				//1
				//w8(pjs.joy[PJAI_X1]);
				//joyy
				//1
				//w8(pjs.joy[PJAI_Y1]);

				//not used
				//1
				w8(0x80);
				//1
				w8(0x80);
			}

			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			//uhhh do nothing?
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceReset");
			return MDRS_DeviceReply;

		case MDCF_MICControl:
		{
			//LOGD("maple_microphone::dma handling MDCF_MICControl %d\n",cmd);
			//MONEY
			u32 function=r32();
			//LOGD("maple_microphone::dma MDCF_MICControl function (1st word) %#010x\n", function);
			//LOGD("maple_microphone::dma MDCF_MICControl words: %d\n", dma_count_in);

			switch(function)
			{
			case MFID_4_Mic:
			{
				//MAGIC HERE
				//http://dcemulation.org/phpBB/viewtopic.php?f=34&t=69600
				// <3 <3 BlueCrab <3 <3
				/*
				2nd word               What it does:
				0x0000??03          Sets the amplifier gain, ?? can be from 00 to 1F
									0x0f = default
				0x00008002          Enables recording
				0x00000001          Returns sampled data while recording is enabled
									While not enabled, returns status of the mic.
				0x00000002          Disables recording
				 *
				 */
				u32 secondword=r32();
				//LOGD("maple_microphone::dma MDCF_MICControl subcommand (2nd word) %#010x\n", subcommand);

				u32 subcommand = secondword & 0xFF; //just get last byte for now, deal with params later

				//LOGD("maple_microphone::dma MDCF_MICControl (3rd word) %#010x\n", r32());
				//LOGD("maple_microphone::dma MDCF_MICControl (4th word) %#010x\n", r32());
				switch(subcommand)
				{
				case 0x01:
				{
					//printf("maple_microphone::dma MDCF_MICControl someone wants some data! (2nd word) %#010x\n", secondword);

					w32(MFID_4_Mic);

					//from what i can tell this is up to spec but results in transmit again
					//w32(secondword);

					//32 bit header
					w8(0x04);//status (just the bit for recording)
					w8(0x0f);//gain (default)
					w8(0);//exp ?
					if(get_mic_data(micdata)){
						w8(240);//ct (240 samples)
						wptr(micdata, SIZE_OF_MIC_DATA);
					}else
					{
						w8(0);
					}

					return MDRS_DataTransfer;
				}
				case 0x02:
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl toggle recording %#010x", secondword);
					return MDRS_DeviceReply;
				case 0x03:
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl set gain %#010x", secondword);
					return MDRS_DeviceReply;
				case MDRE_TransmitAgain:
					WARN_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl MDRE_TransmitAgain");
					//apparently this doesnt matter
					//wptr(micdata, SIZE_OF_MIC_DATA);
					return MDRS_DeviceReply;//MDRS_DataTransfer;
				default:
					INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED secondword %#010x", secondword);
               return MDRE_UnknownFunction;
				}
			}
			default:
				INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED function %#010x", function);
            return MDRE_UnknownFunction;
			}
		}

		default:
			INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED MAPLE COMMAND %d", cmd);
         return MDRE_UnknownCmd;
		}
	}	
};

struct maple_sega_purupuru : maple_base
{
   u16 AST = 19, AST_ms = 5000;
   u32 VIBSET;

   virtual MapleDeviceType get_device_type()
   {
	  return MDT_PurupuruPack;
   }

   virtual bool maple_serialize(void **data, unsigned int *total_size)
   {
      LIBRETRO_S(AST);
      LIBRETRO_S(AST_ms);
      LIBRETRO_S(VIBSET);
      return true ;
   }
   virtual bool maple_unserialize(void **data, unsigned int *total_size)
   {
      LIBRETRO_US(AST);
      LIBRETRO_US(AST_ms);
      LIBRETRO_US(VIBSET);
      return true ;
   }
   virtual u32 dma(u32 cmd)
   {
      switch (cmd)
      {
         case MDC_DeviceRequest:
            //caps
            //4
            w32(MFID_8_Vibration);

            //struct data
            //3*4
            w32(0x00000101);
            w32(0);
            w32(0);

            //1	area code
            w8(0xFF);

            //1	direction
            w8(0);

            //30
            wstr(maple_sega_purupuru_name, 30);

            //60
            wstr(maple_sega_brand, 60);

            //2
            w16(0x00C8);

            //2
            w16(0x0640);

            return MDRS_DeviceStatus;

            //get last vibration
         case MDCF_GetCondition:

            w32(MFID_8_Vibration);

            w32(VIBSET);

            return MDRS_DataTransfer;

         case MDCF_GetMediaInfo:

            w32(MFID_8_Vibration);

            // PuruPuru vib specs
            w32(0x3B07E010);

            return MDRS_DataTransfer;

         case MDCF_BlockRead:

            w32(MFID_8_Vibration);
            w32(0);

            w16(2);
            w16(AST);

            return MDRS_DataTransfer;

         case MDCF_BlockWrite:

            //Auto-stop time
            AST = dma_buffer_in[10];
            AST_ms = AST * 250 + 250;

            return MDRS_DeviceReply;

         case MDCF_SetCondition:

            VIBSET = *(u32*)&dma_buffer_in[4];
            //Do the rumble thing!
            config->SetVibration(VIBSET, AST_ms);

            return MDRS_DeviceReply;

         default:
			INFO_LOG(MAPLE, "UNKOWN MAPLE COMMAND %d", cmd);
            return MDRE_UnknownCmd;
      }
   }
};

char EEPROM[0x100];
bool EEPROM_loaded = false;

u8 kb_shift; 		// shift keys pressed (bitmask)
u8 kb_led; 			// leds currently lit
u8 kb_key[6]={0};	// normal keys pressed

struct maple_keyboard : maple_base
{
	virtual MapleDeviceType get_device_type()
	{
		return MDT_Keyboard;
	}

	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
			//caps
			//4
         w32(MFID_6_Keyboard);
 			//struct data
			//3*4
         w32(0x80000502);	// US, 104 keys
			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
         // Product name (30)
			wstr(maple_sega_kbd_name, 30);

         // License (60)
			wstr(maple_sega_brand, 60);

         // Low-consumption standby current (2)
			w16(0x01AE);

         // Maximum current consumption (2)
			w16(0x01F5);
         
         return MDRS_DeviceStatus;
 		case MDCF_GetCondition:
         w32(MFID_6_Keyboard);
			//struct data
			//int8 shift          ; shift keys pressed (bitmask)	//1
			w8(kb_shift);
			//int8 led            ; leds currently lit			//1
			w8(kb_led);
			//int8 key[6]         ; normal keys pressed			//6
         for (int i = 0; i < 6; i++)
			{
				w8(kb_key[i]);
			}
         return MDRS_DataTransfer;
 		default:
			INFO_LOG(MAPLE, "Keyboard: unknown MAPLE COMMAND %d", cmd);
         return MDRE_UnknownCmd;
		}
	}
};

// Mouse buttons
// bit 0: Button C
// bit 1: Right button (B)
// bit 2: Left button (A)
// bit 3: Wheel button
u32 mo_buttons[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
// Relative mouse coordinates [-512:511]
f32 mo_x_delta[4];
f32 mo_y_delta[4];
f32 mo_wheel_delta[4];
// Absolute mouse coordinates
// Range [0:639] [0:479]
// but may be outside this range if the pointer is offscreen or outside the 4:3 window.
f32 mo_x_abs[4];
f32 mo_y_abs[4];

struct maple_mouse : maple_base
{
	virtual MapleDeviceType get_device_type()
	{
		return MDT_Mouse;
	}

	static u16 mo_cvt(f32 delta)
	{
		delta += 0x200;
		if (delta<=0)
			delta=0;
		else if (delta>0x3FF)
			delta=0x3FF;

		return (u16)lroundf(delta);
	}
 	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
			//caps
			//4
			w32(MFID_9_Mouse);
 			//struct data
			//3*4
			w32(0x00070E00);	// Mouse, 3 buttons, 3 axes
			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
			// Product name (30)
			wstr(maple_sega_mouse_name, 30);

 			// License (60)
			wstr(maple_sega_brand, 60);

 			// Low-consumption standby current (2)
			w16(0x0069);
 			// Maximum current consumption (2)
			w16(0x0120);

			return MDRS_DeviceStatus;

 		case MDCF_GetCondition:
		    {
		       u32 buttons;
		       f32 delta_x, delta_y, delta_wheel;
			   config->GetMouse(&buttons, &delta_x, &delta_y, &delta_wheel);

			   w32(MFID_9_Mouse);
			   //struct data
			   //int32 buttons       ; digital buttons bitfield (little endian)
			   w32(buttons);
			   //int16 axis1         ; horizontal movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_x));
			   //int16 axis2         ; vertical movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_y));
			   //int16 axis3         ; mouse wheel movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_wheel));
			   //int16 axis4         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis5         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis6         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis7         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis8         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
		    }

			return MDRS_DataTransfer;

 		default:
			INFO_LOG(MAPLE, "Mouse: unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_lightgun : maple_base
{
	virtual u32 transform_kcode(u32 kcode) {
		return kcode | 0xFF01;
	}

   virtual MapleDeviceType get_device_type()
   {
	  return MDT_LightGun;
   }

   virtual u32 dma(u32 cmd)
   {
	  switch (cmd)
	  {
	  case MDC_DeviceRequest:
		 //caps
		 //4
		 w32(MFID_7_LightGun | MFID_0_Input);

		 //struct data
		 //3*4
		 w32(0);				// Light gun
		 w32(0xFE000000);	// Controller
		 w32(0);
		 //1	area code
		 w8(0x01);		// FF: Worldwide, 01: North America
		 //1	direction
		 w8(0);
		 // Product name (30)
		 wstr(maple_sega_lightgun_name, 30);

		 // License (60)
		 wstr(maple_sega_brand, 60);

		 // Low-consumption standby current (2)
		 w16(0x0069);	// 10.5 mA

		 // Maximum current consumption (2)
		 w16(0x0120);	// 28.8 mA

		 return MDRS_DeviceStatus;

	  case MDCF_GetCondition:
	  {
		 PlainJoystickState pjs;
		 config->GetInput(&pjs);

		 //caps
		 //4
		 w32(MFID_0_Input);

		 //state data
		 //2 key code
			w16(transform_kcode(pjs.kcode));

		 //not used
		 //2
		 w16(0xFFFF);

		 //not used
		 //4
		 w32(0x80808080);
	  }

	  return MDRS_DataTransfer;

	  default:
			INFO_LOG(MAPLE, "Light gun: unknown MAPLE COMMAND %d", cmd);
		 return MDRE_UnknownCmd;
	  }
   }

   virtual void get_lightgun_pos()
   {
	  f32 x, y;
	  config->GetAbsolutePosition(&x, &y);
	  read_lightgun_position(x + 0.5f, y + 0.5f);
   }
};

struct atomiswave_lightgun : maple_lightgun
{
	virtual u32 transform_kcode(u32 kcode) override {
		return (kcode & AWAVE_TRIGGER_KEY) == 0 ? ~AWAVE_BTN0_KEY : ~0;
	}
};

extern u32 kcode[4];
extern s8 joyx[4], joyy[4], joyrx[4], joyry[4];
extern u8 rt[4], lt[4];
extern char eeprom_file[PATH_MAX];

/*
   Sega JVS I/O board
*/
struct maple_naomi_jamma;

class jvs_io_board
{
public:
	jvs_io_board(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
	{
		this->node_id = node_id;
		this->parent = parent;
		coin_count[0] = 0;
		coin_chute[0] = false;
		coin_count[1] = 0;
		coin_chute[1] = false;
		this->first_player = first_player;
	}
	virtual ~jvs_io_board() = default;

	u32 handle_jvs_message(u8 *buffer_in, u32 length_in, u8 *buffer_out);
	bool maple_serialize(void **data, unsigned int *total_size);
	bool maple_unserialize(void **data, unsigned int *total_size);

	bool lightgun_as_analog = false;

protected:
	virtual const char *get_id() = 0;
	virtual u32 remap_buttons(u32 keycode) { return keycode; }
	virtual u16 read_analog_axis(int player_num, int player_axis, bool inverted);
	virtual u32 read_digital_in(int player_num) {
		u32 keycode = player_num > 3 ? 0 : ~kcode[player_num];
		return remap_buttons(keycode);
	}
	virtual void write_digital_out(int count, u8 *data) { }

	u32 player_count = 0;
	u32 digital_in_count = 0;
	u32 coin_input_count = 0;
	u32 analog_count = 0;
	u32 encoder_count = 0;
	u32 light_gun_count = 0;
	u32 output_count = 0;
	bool init_in_progress = false;

private:
	u16 read_rotary_encoder(f32 &encoder_value, f32 delta_value);

	u8 node_id = 0;
	maple_naomi_jamma *parent;
	u32 coin_count[2];
	bool coin_chute[2];
	u8 first_player;
};

// Most common JVS board
class jvs_837_13551 : public jvs_io_board
{
public:
	jvs_837_13551(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 2;
		digital_in_count = 13;
		coin_input_count = 2;
		analog_count = 8;
		output_count = 6;
	}
protected:
	virtual const char *get_id() override { return "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10"; }
};

class jvs_837_13551_noanalog : public jvs_837_13551
{
public:
	jvs_837_13551_noanalog(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_837_13551(node_id, parent, first_player)
	{
		analog_count = 0;
	}
};

// Same in 4-player mode
class jvs_837_13551_4P : public jvs_837_13551
{
public:
	jvs_837_13551_4P(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_837_13551(node_id, parent, first_player)
	{
		player_count = 4;
		digital_in_count = 12;
		coin_input_count = 4;
		analog_count = 0;
	}
};

// Rotary encoders 2nd board
// Virtua Golf, Outtrigger, Shootout Pool
class jvs_837_13938 : public jvs_io_board
{
public:
	jvs_837_13938(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 1;
		digital_in_count = 9;
		encoder_count = 4;
		output_count = 8;
	}
protected:
	virtual const char *get_id() override { return "SEGA ENTERPRISES,LTD.;837-13938 ENCORDER BD  ;Ver0.01;99/08"; }
};

// Sega Marine Fishing
class jvs_837_13844 : public jvs_io_board
{
public:
	jvs_837_13844(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 2;
		coin_input_count = 2;
		digital_in_count = 12;
		analog_count = 8;
		output_count = 22;
	}
protected:
	virtual const char *get_id() override { return "SEGA ENTERPRISES,LTD.;837-13844-01 I/O CNTL BD2 ;Ver1.00;99/07"; }

};

class jvs_837_13844_encoders : public jvs_837_13844
{
public:
	jvs_837_13844_encoders(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_837_13844(node_id, parent, first_player)
	{
		digital_in_count = 8;
		encoder_count = 4;
	}
};

class jvs_837_13844_touch : public jvs_837_13844
{
public:
	jvs_837_13844_touch(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_837_13844(node_id, parent, first_player)
	{
		light_gun_count = 1;
	}
};

// Wave Runner GP: fake the drive board
class jvs_837_13844_wrungp : public jvs_837_13844
{
public:
	jvs_837_13844_wrungp(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_837_13844(node_id, parent, first_player)
	{
	}
protected:
	virtual u32 read_digital_in(int player_num) override {
		u32 rv = jvs_837_13844::read_digital_in(player_num);

		// The drive board RX0-7 is connected to the following player inputs
		if (player_num == 0)
		{
			rv |= NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
			if (drive_board & 16)
				rv &= ~NAOMI_BTN5_KEY;
			if (drive_board & 32)
				rv &= ~NAOMI_BTN4_KEY;
			if (drive_board & 64)
				rv &= ~NAOMI_BTN3_KEY;
			if (drive_board & 128)
				rv &= ~NAOMI_BTN2_KEY;
		}
		else if (player_num == 1)
		{
			rv |= NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
			if (drive_board & 1)
				rv &= ~NAOMI_BTN5_KEY;
			if (drive_board & 2)
				rv &= ~NAOMI_BTN4_KEY;
			if (drive_board & 4)
				rv &= ~NAOMI_BTN3_KEY;
			if (drive_board & 8)
				rv &= ~NAOMI_BTN2_KEY;
		}
		return rv;
	}

	virtual void write_digital_out(int count, u8 *data) override {
		if (count != 3)
			return;

		// The drive board TX0-7 is connected to outputs 15-22
		// shifting right by 2 to get the last 8 bits of the output
		u16 out = (data[1] << 6) | (data[2] >> 2);
		// reverse
		out = (out & 0xF0) >> 4 | (out & 0x0F) << 4;
		out = (out & 0xCC) >> 2 | (out & 0x33) << 2;
		out = (out & 0xAA) >> 1 | (out & 0x55) << 1;

		if (out == 0xff)
			drive_board = 0xff;
		else if ((out & 0xf) == 0xf)
		{
			out >>= 4;
			if (out > 7)
				drive_board = 0xff & ~(1 << (14 - out));
			else
				drive_board = 0xff & ~(1 << out);
		}
		else if ((out & 0xf0) == 0xf0)
		{
			out &= 0xf;
			if (out > 7)
				drive_board = 0xff & ~(1 << (out - 7));
			else
				drive_board = 0xff & ~(1 << (7 - out));
		}
	}

private:
	u8 drive_board = 0;
};

// Ninja assault
class jvs_namco_jyu : public jvs_io_board
{
public:
	jvs_namco_jyu(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 2;
		coin_input_count = 2;
		digital_in_count = 12;
		output_count = 16;
		light_gun_count = 2;
	}
protected:
	virtual const char *get_id() override { return "namco ltd.;JYU-PCB;Ver1.00;JPN,2Coins 2Guns"; }
	virtual u32 remap_buttons(u32 keycode)
	{
	   // Swap button 1 and 4
	   u16 mapped = keycode | NAOMI_BTN1_KEY | NAOMI_BTN4_KEY;
	   if (!(keycode & NAOMI_BTN1_KEY))
		  mapped &= ~NAOMI_BTN4_KEY;
	   if (!(keycode & NAOMI_BTN4_KEY))
		  mapped &= ~NAOMI_BTN1_KEY;
	   return mapped;
	}
};

// Mazan
class jvs_namco_fcb : public jvs_io_board
{
public:
	jvs_namco_fcb(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 1;
		coin_input_count = 2;
		digital_in_count = 16;
		output_count = 6;
		light_gun_count = 1;
		analog_count = 7;
		encoder_count = 2;
	}
protected:
	virtual const char *get_id() override { return "namco ltd.;FCB;Ver1.0;JPN,Touch Panel & Multipurpose"; }

	virtual u16 read_analog_axis(int player_num, int player_axis, bool inverted) override {
		if (init_in_progress)
			return 0;
		if (mo_x_abs[player_num] < 0 || mo_x_abs[player_num] > 639 || mo_y_abs[player_num] < 0 || mo_y_abs[player_num] > 479)
			return 0;
		else
			return 0x8000;
	}
};

// Gun Survivor
class jvs_namco_fca : public jvs_io_board
{
public:
	jvs_namco_fca(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 1;
		coin_input_count = 1; // 2 makes bios crash
		digital_in_count = 16;
		output_count = 6;
		analog_count = 7;
		encoder_count = 2;
	}
protected:
	virtual const char *get_id() override { return "namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder"; }
};

// World Kicks
class jvs_namco_v226 : public jvs_io_board
{
public:
	jvs_namco_v226(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 1;
		digital_in_count = 16;
		coin_input_count = 1;
		analog_count = 12;
		output_count = 6;
	}
protected:
	virtual const char *get_id() override { return "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10"; }

	virtual u32 read_digital_in(int player_num) override {
		u32 keycode1 = ~kcode[0];
		u32 keycode2 = ~kcode[1];
		u32 keycode3 = ~kcode[2];
		u32 keycode4 = ~kcode[3];
				// main button
		return ((keycode1 & NAOMI_BTN0_KEY) << 6)		// start
				| ((keycode2 & NAOMI_BTN0_KEY) << 2)	// left
				| ((keycode3 & NAOMI_BTN0_KEY) << 1)	// right
				| ((keycode4 & NAOMI_BTN0_KEY) >> 1)	// btn1
				// soft kick
//				| ((~keycode1 & NAOMI_BTN1_KEY) >> 1)	// btn2
//				| ((~keycode2 & NAOMI_BTN1_KEY) >> 3)	// btn4
//				| ((~keycode3 & NAOMI_BTN1_KEY) >> 5)	// btn6
//				| ((~keycode4 & NAOMI_BTN1_KEY) >> 7)	// test?
//				// hard kick
//				| ((~keycode1 & NAOMI_BTN2_KEY) >> 1)	// btn3
//				| ((~keycode2 & NAOMI_BTN2_KEY) >> 3)	// btn5
//				| ((~keycode3 & NAOMI_BTN2_KEY) >> 5)	// btn7
//				| ((~keycode4 & NAOMI_BTN2_KEY) >> 7)	// ?
				// enter
				| ((keycode1 & NAOMI_BTN3_KEY) << 3)	// btn0
				// service menu
				| (keycode1 & (NAOMI_SERVICE_KEY | NAOMI_UP_KEY | NAOMI_DOWN_KEY));
	}

	u16 read_joystick_x(int joy_num)
	{
		s8 axis_x = joyx[joy_num];
		axis_y = joyy[joy_num];
		limit_joystick_magnitude<64>(axis_x, axis_y);
		return (0xff - axis_x) << 8;
	}

	u16 read_joystick_y(int joy_num)
	{
		return (0xff - axis_y) << 8;
	}

	virtual u16 read_analog_axis(int player_num, int player_axis, bool inverted) override {
		switch (player_num)
		{
		case 0:
			switch (player_axis)
			{
			case 0:
				return read_joystick_x(0);
			case 1:
				return read_joystick_y(0);
			case 2:
				return read_joystick_x(1);
			case 3:
				return read_joystick_y(1);
			}
			break;

		case 1:
			switch (player_axis)
			{
			case 0:
				return read_joystick_x(2);
			case 1:
				return read_joystick_y(2);
			case 2:
				return read_joystick_x(3);
			case 3:
				return read_joystick_y(3);
			}
			break;

		case 2:
			switch (player_axis)
			{
			case 0:
				return rt[0] << 8;
			case 1:
				return rt[1] << 8;
			case 2:
				return rt[2] << 8;
			case 3:
				return rt[3] << 8;
			}
			break;
		}
		return 0x8000;
	}

private:
	s8 axis_y = 0;
};

// World Kicks PCB
class jvs_namco_v226_pcb : public jvs_io_board
{
public:
	jvs_namco_v226_pcb(u8 node_id, maple_naomi_jamma *parent, int first_player = 0)
		: jvs_io_board(node_id, parent, first_player)
	{
		player_count = 2;
		digital_in_count = 16;
		coin_input_count = 1;
		analog_count = 12;
		output_count = 6;
	}
protected:
	virtual const char *get_id() override { return "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10"; }

	virtual u32 read_digital_in(int player_num) override {
		u32 keycode = ~kcode[player_num];
		u8 trigger = rt[player_num] >> 2;
				// Ball button
		return ((trigger & 0x20) << 3) | ((trigger & 0x10) << 5) | ((trigger & 0x08) << 7)
				| ((trigger & 0x04) << 9) | ((trigger & 0x02) << 11) | ((trigger & 0x01) << 13)
				// other buttons
				| (keycode & (NAOMI_SERVICE_KEY | NAOMI_TEST_KEY | NAOMI_START_KEY))
				| ((keycode & NAOMI_BTN0_KEY) >> 4);		// remap button4 to button0 (change button)
	}

	u16 read_joystick_x(int joy_num)
	{
		s8 axis_x = joyx[joy_num];
		axis_y = joyy[joy_num];
		limit_joystick_magnitude<48>(axis_x, axis_y);
		return axis_x << 8;
	}

	u16 read_joystick_y(int joy_num)
	{
		return (0xff - axis_y) << 8;
	}

	virtual u16 read_analog_axis(int player_num, int player_axis, bool inverted) override {
		switch (player_num)
		{
		case 0:
			switch (player_axis)
			{
			case 0:
				return read_joystick_x(0);
			case 1:
				return read_joystick_y(0);
			}
			break;

		case 1:
			switch (player_axis)
			{
			case 0:
				return read_joystick_x(1);
			case 1:
				return read_joystick_y(1);
			}
			break;
		}
		return 0x8000;
	}

private:
	s8 axis_y = 0;
};

struct maple_naomi_jamma : maple_sega_controller
{
	const u8 ALL_NODES = 0xff;

	std::vector<jvs_io_board *> io_boards;
	bool crazy_mode = false;

	u8 jvs_repeat_request[32][256];
	u8 jvs_receive_buffer[32][258];
	u32 jvs_receive_length[32] = { 0 };

	maple_naomi_jamma()
	{
	}
	virtual ~maple_naomi_jamma()
	{
		EEPROM_loaded = false;
	}

	void create_io_boards()
	{
	   if (!io_boards.empty())
		  return;
	   switch (settings.mapping.JammaSetup)
	   {
	   case JVS::Default:
	   default:
		  io_boards.emplace_back(new jvs_837_13551(1, this));
		  break;
	   case JVS::FourPlayers:
		  io_boards.emplace_back(new jvs_837_13551_4P(1, this));
		  break;
	   case JVS::RotaryEncoders:
		  io_boards.emplace_back(new jvs_837_13938(1, this));
		  io_boards.emplace_back(new jvs_837_13551(2, this));
		  break;
	   case JVS::OutTrigger:
		  io_boards.emplace_back(new jvs_837_13938(1, this));
		  io_boards.emplace_back(new jvs_837_13551_noanalog(2, this));
		  break;
	   case JVS::SegaMarineFishing:
		  io_boards.emplace_back(new jvs_837_13844(1, this));
		  break;
	   case JVS::DualIOBoards4P:
		  io_boards.emplace_back(new jvs_837_13551(1, this));
		  io_boards.emplace_back(new jvs_837_13551(2, this, 2));
		  break;
	   case JVS::LightGun:
		  io_boards.emplace_back(new jvs_namco_jyu(1, this));
		  break;
	   case JVS::LightGunAsAnalog:
		  // Regular board sending lightgun coords as axis 0/1
		  io_boards.emplace_back(new jvs_837_13551(1, this));
		  io_boards.back()->lightgun_as_analog = true;
		  break;
	   case JVS::Mazan:
		  io_boards.emplace_back(new jvs_namco_fcb(1, this));
		  io_boards.emplace_back(new jvs_namco_fcb(2, this));
		  break;
	   case JVS::GunSurvivor:
		  io_boards.emplace_back(new jvs_namco_fca(1, this));
		  break;
	   case JVS::DogWalking:
		  io_boards.emplace_back(new jvs_837_13844_encoders(1, this));
		  break;
	   case JVS::TouchDeUno:
		  io_boards.emplace_back(new jvs_837_13844_touch(1, this));
		  break;
	   case JVS::WorldKicks:
		  io_boards.emplace_back(new jvs_namco_v226(1, this));
		  break;
	   case JVS::WorldKicksPCB:
		  io_boards.emplace_back(new jvs_namco_v226_pcb(1, this));
		  break;
	   case JVS::WaveRunnerGP:
		  io_boards.emplace_back(new jvs_837_13844_wrungp(1, this));
		  break;
	   }
	}

	virtual MapleDeviceType get_device_type()
	{
		return MDT_NaomiJamma;
	}

	u8 sense_line(u32 node_id)
	{
		bool last_node = node_id == io_boards.size();
		return last_node ? 0x8E : 0x8F;
	}

	void send_jvs_message(u32 node_id, u32 channel, u32 length, u8 *data)
	{
		if (node_id - 1 < io_boards.size())
		{
			u8 temp_buffer[256];
			u32 out_len = io_boards[node_id - 1]->handle_jvs_message(data, length, temp_buffer);
			if (out_len > 0)
			{
				u8 *pbuf = &jvs_receive_buffer[channel][jvs_receive_length[channel]];
				if (jvs_receive_length[channel] + out_len + 3 <= sizeof(jvs_receive_buffer[0]))
				{
					if (crazy_mode)
					{
						pbuf[0] = 0x00;		// ? 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed
						pbuf[1] = out_len;
						memcpy(&pbuf[2], temp_buffer, out_len);
						jvs_receive_length[channel] += out_len + 2;
					}
					else
					{
						pbuf[0] = node_id;
						pbuf[1] = 0x00;		// 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed
						pbuf[2] = out_len;
						memcpy(&pbuf[3], temp_buffer, out_len);
						jvs_receive_length[channel] += out_len + 3;
					}
				}
			}
		}
	}

	void send_jvs_messages(u32 node_id, u32 channel, bool use_repeat, u32 length, u8 *data, bool repeat_first)
	{
		u8 temp_buffer[256];
		if (data)
		{
			memcpy(temp_buffer, data, length);
		}
		if (node_id == ALL_NODES)
		{
			for (int i = 0; i < io_boards.size(); i++)
				send_jvs_message(i + 1, channel, length, temp_buffer);
		}
		else if (node_id >= 1 && node_id <= 32)
		{
			u32 repeat_len = jvs_repeat_request[node_id - 1][0];
			if (use_repeat && repeat_len > 0)
			{
				if (repeat_first)
				{
				   memmove(temp_buffer + repeat_len, temp_buffer, length);
				   memcpy(temp_buffer, &jvs_repeat_request[node_id - 1][1], repeat_len);
				}
				else
				{
				   memcpy(temp_buffer + length, &jvs_repeat_request[node_id - 1][1], repeat_len);
				}
				length += repeat_len;
			}
			send_jvs_message(node_id, channel, length, temp_buffer);
		}
	}

	void receive_jvs_messages(u32 channel)
	{
	   if (dma_count_in == 0)
	   {
		  w8(MDRS_JVSReply);
		  w8(0);
		  w8(0x20);
		  w8(0x00);

		  return;
	   }
	   u32 dword_length = (jvs_receive_length[channel] + 0x10 + 3 - 1) / 4 + 1;

	   w8(MDRS_JVSReply);
	   w8(0x00);
	   w8(0x20);
	   if (jvs_receive_length[channel] == 0)
	   {
		  w8(0x05);
		  w8(0x32);
	   }
	   else
	   {
		  w8(dword_length);
		  w8(0x16);
	   }
	   w8(0xff);
	   w8(0xff);
	   w8(0xff);
	   w32(0xffffff00);
	   w32(0);
	   w32(0);

	   if (jvs_receive_length[channel] == 0)
	   {
		  w32(0);
		  return;
	   }

	   w8(0);
	   w8(channel);
	   if (crazy_mode)
		  w8(0x8E);
	   else
		  w8(sense_line(jvs_receive_buffer[channel][0]));	// bit 0 is sense line level. If set during F1 <n>, more I/O boards need addressing

	   memcpy(dma_buffer_out, jvs_receive_buffer[channel], jvs_receive_length[channel]);
	   dma_buffer_out += dword_length * 4 - 0x10 - 3;
	   *dma_count_out += dword_length * 4 - 0x10 - 3;
	   jvs_receive_length[channel] = 0;
	}

	void handle_86_subcommand()
	{
	   if (dma_count_in == 0)
	   {
		  w8(MDRS_JVSReply);
		  w8(0);
		  w8(0x20);
		  w8(0x00);
		  return;
	   }
	   u32 subcode = dma_buffer_in[0];

		// CT fw uses 13 as a 17, and 17 as 13 and also uses 19
		if (crazy_mode)
		{
			switch (subcode)
			{
			case 0x13:
				subcode = 0x17;
				break;
			case 0x17:
				subcode = 0x13;
				break;
			}
		}
		u8 node_id = 0;
		u8 *cmd = NULL;
		u32 len = 0;
		u8 channel = 0;
		if (dma_count_in >= 3)
		{
			if (dma_buffer_in[1] > 31 && dma_buffer_in[1] != 0xff && dma_count_in >= 8)	// TODO what is this?
			{
				node_id = dma_buffer_in[6];
				len = dma_buffer_in[7];
				cmd = &dma_buffer_in[8];
				channel = dma_buffer_in[5] & 0x1f;
			}
			else
			{
				node_id = dma_buffer_in[1];
				len = dma_buffer_in[2];
				cmd = &dma_buffer_in[3];
			}
		}

		switch (subcode)
		{
			case 0x13:	// Store repeated request
				if (len > 0 && node_id > 0 && node_id <= 0x1f)
				{
					INFO_LOG(MAPLE, "JVS node %d: Storing %d cmd bytes", node_id, len);
					jvs_repeat_request[node_id - 1][0] = len;
					memcpy(&jvs_repeat_request[node_id - 1][1], cmd, len);
				}
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(dma_buffer_in[0] + 1);	// subcommand + 1
				w8(0);
				w8(len + 1);
				w8(0);
				break;

			case 0x15:	// Receive JVS data
				receive_jvs_messages(dma_buffer_in[1]);
				break;

			case 0x17:	// Transmit without repeat
				jvs_receive_length[channel] = 0;
				send_jvs_messages(node_id, channel, false, len, cmd, false);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(0x8E);	//sense_line(node_id));
				w8(0);
				break;

			case 0x19:	// Transmit with repeat
				jvs_receive_length[channel] = 0;
				send_jvs_messages(node_id, channel, true, len, cmd, true);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(sense_line(node_id));
				w8(0);
				break;

			case 0x21:	// Transmit with repeat
			   jvs_receive_length[channel] = 0;
			   send_jvs_messages(node_id, channel, true, len, cmd, false);
			   w8(MDRS_JVSReply);
			   w8(0);
			   w8(0x20);
			   w8(0x01);
			   w8(0x18);	// always
			   w8(channel);
			   w8(sense_line(node_id));
			   w8(0);
			   break;

			case 0x35:	// Receive then transmit with repeat (15 then 27)
				receive_jvs_messages(channel);
				// FALLTHROUGH

			case 0x27:	// Transmit with repeat
				{
					jvs_receive_length[channel] = 0;

					u32 cmd_count = dma_buffer_in[6];
					u32 idx = 7;
					for (int i = 0; i < cmd_count; i++)
					{
						node_id = dma_buffer_in[idx];
						len = dma_buffer_in[idx + 1];
						cmd = &dma_buffer_in[idx + 2];
						idx += len + 2;

						send_jvs_messages(node_id, channel, true, len, cmd, false);
					}

					w8(MDRS_JVSReply);
					w8(0);
					w8(0x20);
					w8(0x01);
					w8(0x26);
					w8(channel);
					w8(sense_line(node_id));
					w8(0);
				}
				break;

			case 0x33:	// Receive then transmit with repeat (15 then 21)
				receive_jvs_messages(channel);
				send_jvs_messages(node_id, channel, true, len, cmd, false);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(sense_line(node_id));
				w8(0);
				break;

			case 0x0B:	//EEPROM write
			{
				int address = dma_buffer_in[1];
				int size = dma_buffer_in[2];
				DEBUG_LOG(MAPLE, "EEprom write %08X %08X\n", address, size);
				//printState(Command,buffer_in,buffer_in_len);
				memcpy(EEPROM + address, dma_buffer_in + 4, size);

				FILE* f = fopen(eeprom_file, "wb");
				if (f)
				{
				   fwrite(EEPROM, 1, 0x80, f);
				   fclose(f);
				}
				else
					WARN_LOG(MAPLE, "Cannot save EEPROM to file %s", eeprom_file);

				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x01);
				memcpy(dma_buffer_out, EEPROM, 4);
				dma_buffer_out += 4;
				*dma_count_out += 4;
			}
			break;

			case 0x3:	//EEPROM read
			{
				FILE* f = fopen(eeprom_file, "rb");
				if (f)
				{
				   fread(EEPROM, 1, 0x80, f);
				   fclose(f);
				   DEBUG_LOG(MAPLE, "Loaded EEPROM from %s", eeprom_file);
				}
				else if (naomi_default_eeprom != NULL)
					memcpy(EEPROM, naomi_default_eeprom, 0x80);

				//printf("EEprom READ\n");
				int address = dma_buffer_in[1];
				//printState(Command,buffer_in,buffer_in_len);
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x20);
				memcpy(dma_buffer_out, EEPROM + address, 0x80);
				dma_buffer_out += 0x80;
				*dma_count_out += 0x80;
			}
			break;

			case 0x31:	// DIP switches
			{
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x05);

				w8(0x32);
				w8(0xff);		// in(0)
				w8(0xff);		// in(1)
				w8(0xff);		// in(2)

				w8(0x00);
				w8(0xff);		// in(4)
				w8(enable_naomi_15khz_dipswitch ? 0x00 : 0xff);		// in(5) bit0: 1=VGA, 0=NTSCi
				w8(0xff);		// in(6)

				w32(0x00);
				w32(0x00);
				w32(0x00);
			}
			break;

			//case 0x3:
			//	break;

			case 0x1:
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x01);

				w8(0x2);
				w8(0x0);
				w8(0x0);
				w8(0x0);
				break;

			default:
				INFO_LOG(MAPLE, "JVS: Unknown 0x86 sub-command %x", subcode);
				w8(MDRE_UnknownCmd);
				w8(0x00);
				w8(0x20);
				w8(0x00);
				break;
		}
	}

	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out)
	{
#ifdef DUMP_JVS
	   printf("JVS IN: ");
	   u8 *p = (u8*)buffer_in;
	   for (int i = 0; i < buffer_in_len; i++) printf("%02x ", *p++);
	   printf("\n");
#endif
	   create_io_boards();

		u32 out_len = 0;
		dma_buffer_out = (u8 *)buffer_out;
		dma_count_out = &out_len;

		dma_buffer_in = (u8 *)buffer_in + 4;
		dma_count_in = buffer_in_len - 4;

		u32 cmd = *(u8*)buffer_in;
		switch (cmd)
		{
			case MDC_JVSCommand:
				handle_86_subcommand();
				break;

			case MDC_JVSUploadFirmware:
			{
				static u8 *ram;
				static XXH32_state_t* state;

				if (ram == NULL)
					ram = (u8 *)calloc(0x10000, 1);

				if (dma_buffer_in[1] == 0xff)
				{
					u32 hash = XXH32(ram, 0x10000, 0);
					LOGJVS("JVS Firmware hash %08x\n", hash);
					if (hash == 0xa7c50459			// CT
						  || hash == 0xae841e36)	// HOTD2
						crazy_mode = true;
					else
						crazy_mode = false;
#ifdef DUMP_JVS_FW
					FILE *fw_dump;
					char filename[128];
					for (int i = 0; ; i++)
					{
						sprintf(filename, "z80_fw_%d.bin", i);
						fw_dump = fopen(filename, "r");
						if (fw_dump == NULL)
						{
							fw_dump = fopen(filename, "w");
							INFO_LOG(MAPLE, "Saving JVS firmware to %s", filename);
							break;
						}
					}
					if (fw_dump)
					{
						fwrite(ram, 1, 0x10000, fw_dump);
						fclose(fw_dump);
					}
#endif
					free(ram);
					ram = NULL;
					for (int i = 0; i < 32; i++)
						jvs_repeat_request[i][0] = 0;

					return MDRS_DeviceReply;
				}
				int xfer_bytes;
				if (dma_buffer_in[0] == 0xff)
					xfer_bytes = 0x1C;
				else
					xfer_bytes = 0x18;
				u16 addr = (dma_buffer_in[2] << 8) + dma_buffer_in[3];
				memcpy(ram + addr, &dma_buffer_in[4], xfer_bytes);
				u8 sum = 0;
				for (int i = 0; i < 0x1C; i++)
					sum += dma_buffer_in[i];

				w8(0x80);	// or 0x81 on bootrom?
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(sum);
				w8(0);
				w8(0);
				w8(0);

				w8(MDRS_DeviceReply);
				w8(0x00);
				w8(0x20);
				w8(0x00);
			}
			break;

		case MDC_JVSGetId:
			{
				const char ID1[] = "315-6149    COPYRIGHT SEGA E";
				const char ID2[] = "NTERPRISES CO,LTD.  1998    ";
				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x07);
				wstr(ID1, 28);

				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x05);
				wstr(ID2, 28);
			}
			break;

		case MDC_DeviceRequest:
			w8(MDRS_DeviceStatus);
			w8(0x00);
			w8(0x20);
			w8(0x00);

			break;

		case MDC_DeviceReset:
			w8(MDRS_DeviceReply);
			w8(0x00);
			w8(0x20);
			w8(0x00);

			break;

		case MDCF_GetCondition:
			w8(MDRE_UnknownCmd);
			w8(0x00);
			w8(0x00);
			w8(0x00);

			break;

		default:
			INFO_LOG(MAPLE, "Unknown Maple command %x", cmd);
			w8(MDRE_UnknownCmd);
			w8(0x00);
			w8(0x00);
			w8(0x00);

			break;
		}

#ifdef DUMP_JVS
		printf("JVS OUT: ");
		p = (u8 *)buffer_out;
		for (int i = 0; i < out_len; i++) printf("%02x ", p[i]);
		printf("\n");
#endif

		return out_len;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
	   LIBRETRO_S(crazy_mode);
	   LIBRETRO_S(jvs_repeat_request);
	   LIBRETRO_S(jvs_receive_length);
	   LIBRETRO_S(jvs_receive_buffer);
	   size_t board_count = io_boards.size();
	   LIBRETRO_S(board_count);
	   for (int i = 0; i < io_boards.size(); i++)
		  io_boards[i]->maple_serialize(data, total_size);

	   return true ;
	}

	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
	   LIBRETRO_US(crazy_mode);
	   LIBRETRO_US(jvs_repeat_request);
	   LIBRETRO_US(jvs_receive_length);
	   LIBRETRO_US(jvs_receive_buffer);
	   create_io_boards();
	   size_t board_count;
	   LIBRETRO_US(board_count);
	   for (int i = 0; i < board_count; i++)
		  io_boards[i]->maple_unserialize(data, total_size);

	   return true;
	}
};

u16 jvs_io_board::read_analog_axis(int player_num, int player_axis, bool inverted)
{
	u16 v;
	switch (player_axis)
	{
	case 0:
		v = joyx[player_num];
		break;
	case 1:
		v = joyy[player_num];
		break;
	case 2:
		v = joyrx[player_num];
		break;
	case 3:
		v = joyry[player_num];
		break;
	case 4:
		v = rt[player_num];
		break;
	case 5:
		v = lt[player_num];
		break;
	default:
		return 0x8000;
	}
	return (inverted ? 0xff - v : v) << 8;
}

u16 jvs_io_board::read_rotary_encoder(f32 &encoder_value, f32 delta_value)
{
   encoder_value = fmod(encoder_value + delta_value, 65536.f);
   while (encoder_value < 0)
	  encoder_value += 65536.f;
   return (u16)(encoder_value + 0.5);
}

#define JVS_OUT(b) buffer_out[length++] = b
#define JVS_STATUS1() JVS_OUT(1)

static int x1delta = 0;
static int x2delta = 0;
static int y1delta = 0; 
static int y2delta = 0;
static bool alt = false;
static bool parry = false;
static bool stabbing = false;

u32 jvs_io_board::handle_jvs_message(u8 *buffer_in, u32 length_in, u8 *buffer_out)
{
	//ERROR_LOG(JVS, "START MESSAGE");
	u8 jvs_cmd = buffer_in[0];
	if (jvs_cmd == 0xF0)		// JVS reset
		// Nothing to do
		return 0;
	if (jvs_cmd == 0xF1 && (length_in < 2 || buffer_in[1] != node_id))
		// Not for us
		return 0;

	u32 length = 0;
	JVS_OUT(0xE0);	// sync
	JVS_OUT(0);		// master node id
	u8& jvs_length = buffer_out[length++];

	switch (jvs_cmd)
	{
	case 0xF1:		// set board address
		JVS_STATUS1();	// status
		JVS_STATUS1();	// report
		JVS_OUT(5);		// ?
		LOGJVS("JVS Node %d address assigned\n", node_id);
		break;

	case 0x10:	// Read ID data
		JVS_STATUS1();	// status
		JVS_STATUS1();	// report
		for (const char *p = get_id(); *p != 0; )
			JVS_OUT(*p++);
		JVS_OUT(0);
		break;

	case 0x11:	// Get command format version
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x11);	// 1.1
		break;

	case 0x12:	// Get JAMMA VIDEO version
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x20);	// 2.0
		break;

	case 0x13:	// Get communication version
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x10);	// 1.0
		break;

	case 0x14:	// Get slave features
		JVS_STATUS1();
		JVS_STATUS1();

		JVS_OUT(1);		// Digital inputs
		JVS_OUT(player_count);
		JVS_OUT(digital_in_count);
		JVS_OUT(0);

		if (coin_input_count > 0)
		{
			JVS_OUT(2);		// Coin inputs
			JVS_OUT(coin_input_count);
			JVS_OUT(0);
			JVS_OUT(0);
		}

		if (analog_count > 0)
		{
			JVS_OUT(3);		// Analog inputs
			JVS_OUT(analog_count);	//   channel count
			JVS_OUT(0x10);	//   16 bits per channel, 0: unknown
			JVS_OUT(0);
		}

		if (encoder_count > 0)
		{
			JVS_OUT(4);		// Rotary encoders
			JVS_OUT(encoder_count);
			JVS_OUT(0);
			JVS_OUT(0);
		}

		if (light_gun_count > 0)
		{
		   JVS_OUT(6);		// Light gun
		   JVS_OUT(16);		//   X bits
		   JVS_OUT(16);		//   Y bits
		   JVS_OUT(light_gun_count);
		}

		JVS_OUT(0x12);	// General output driver
		JVS_OUT(output_count);
		JVS_OUT(0);
		JVS_OUT(0);

		JVS_OUT(0);		// End of list
		break;

	case 0x15:	// Master board ID
		JVS_STATUS1();
		JVS_STATUS1();
		break;

	case 0x70:
		LOGJVS("JVS 0x70: %02x %02x %02x %02x", buffer_in[1], buffer_in[2], buffer_in[3], buffer_in[4]);
		init_in_progress = true;
		JVS_STATUS1();
		JVS_STATUS1();
		if (buffer_in[2] == 3)
		{
			JVS_OUT(0x10);
			for (int i = 0; i < 16; i++)
				JVS_OUT(0x7f);
			if (buffer_in[4] == 0x10 || buffer_in[4] == 0x11)
				init_in_progress = false;
		}
		else
		{
			JVS_OUT(2);
			JVS_OUT(3);
			JVS_OUT(1);
		}
		break;

	default:
		//ERROR_LOG(JVS, "Reading Inputs: %d.", jvs_cmd);
		if (jvs_cmd >= 0x20 && jvs_cmd <= 0x38) // Read inputs and more
		{
			LOGJVS("JVS Node %d: ", node_id);
			PlainJoystickState pjs;
			parent->config->GetInput(&pjs);

			JVS_STATUS1();	// status
			for (int cmdi = 0; cmdi < length_in; )
			{
				switch (buffer_in[cmdi])
				{
				case 0x20:	// Read digital input
					{
						JVS_STATUS1();	// report byte
						//ERROR_LOG(JVS, "btns ");

						u8 global_btns = 0;
						if (!(kcode[0] & NAOMI_TEST_KEY))		// Global test button
						   global_btns |= 0x80;
						//ERROR_LOG(JVS, "glob %02x ", global_btns);
						JVS_OUT(global_btns);		// test, tilt1, tilt2, tilt3, unused, unused, unused, unused

						u32 next_keycode = 0;

						for (int player = 0; player < buffer_in[cmdi + 1]; player++)
						{
						   u32 keycode = read_digital_in(first_player + player);

						   if (naomi_game_inputs != NULL)
						   {
							  u32 new_keycode = 0;
							  for (int i = 0; naomi_game_inputs->buttons[i].mask != 0; i++)
							  {
								 if ((naomi_game_inputs->buttons[i].mask & keycode) != 0
									   && naomi_game_inputs->buttons[i].p2_mask != 0)
								 {
									new_keycode |= naomi_game_inputs->buttons[i].p2_mask;
									keycode &= ~naomi_game_inputs->buttons[i].mask;
								 }
							  }
							  keycode |= next_keycode;
							  next_keycode = new_keycode;
						   }
						   if (settings.mapping.JammaSetup == JVS::Mazan) {
						   		stabbing = false;
								parry = false;
		
						   		/*switch ((keycode >> 8) & 0x03)
						   		{
						   			// (keycode >> 8) & 0xFF
						   			// trigger: 2
						   			// Aux A: 1
						   			// Aux B: N/A
						   			// Aux C: N/A
						   			// Reload: offscreen + 2
						   			// start: 128
						   			case 3:
						   			case 2:
							  			stabbing = true;
							  			break;
							  		case 1:
							  			parry = true;
							  			break;
						   		}
						   		*/
						   		//ERROR_LOG(JVS, "Keycode: %d", keycode);
						   		switch (keycode) {
						   			case 8192: // up 1
						   				y1delta--;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 4096: // down 1
						   				y1delta++;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 2048: // left 1
						   				x1delta--;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 1024: // right 1
						   				x1delta++;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 256: // up 2 - Aux A - 1
						   				y2delta--;
						   				parry = true;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 512: // down 2 - trigger - 2
						   				y2delta++;
						   				stabbing = true;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 1: // left 2
						   				x2delta--;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   			case 32768: // right 2
						   				x2delta++;
						   				//ERROR_LOG(JVS, "Keycode: %d - x1delta: %d, x2delta: %d, y1delta: %d, y2delta: %d, alt: %d", keycode, x1delta, x2delta, y1delta, y2delta, alt);
						   				break;
						   		}


						   	}

						   //ERROR_LOG(JVS, "P%d %02x ", first_player + player + 1, (keycode >> 8) & 0xFF);
						   JVS_OUT(keycode >> 8);
						   if (buffer_in[cmdi + 2] > 1)
						   {
							  LOGJVS("%02x ", keycode & 0xFF);
							  JVS_OUT(keycode);
						   }
						}
						cmdi += 3;
					}
					break;

				case 0x21:	// Read coins
					{
						JVS_STATUS1();	// report byte
						LOGJVS("coins ");
						for (int slot = 0; slot < buffer_in[cmdi + 1]; slot++)
						{
						   if (slot < ARRAY_SIZE(coin_count)
								 && first_player + slot < 4)
						   {
							  if (!(kcode[first_player + slot] & NAOMI_COIN_KEY))
							  {
								 if (!coin_chute[slot])
								 {
									coin_chute[slot] = true;
									coin_count[slot]++;
								 }
							  }
							  else
								 coin_chute[slot] = false;

							  LOGJVS("0:%d ", coin_count[slot]);
							  JVS_OUT((coin_count[slot] >> 8) & 0x3F);		// status (2 highest bits, 0: normal), coin count MSB
							  JVS_OUT(coin_count[slot]);					// coin count LSB
						   }
						   else
						   {
							  LOGJVS("%d:0 ", slot);
							  JVS_OUT(0);
							  JVS_OUT(0);
						   }
						}
						cmdi += 2;
					}
					break;

				case 0x22:	// Read analog inputs
					{
						JVS_STATUS1();	// report byte
						int axis = 0;

						LOGJVS("ana ");
						if (lightgun_as_analog)
						{
							//ERROR_LOG(JVS, "lightgun as analog");
							// Death Crimson / Confidential Mission
							while (axis < buffer_in[cmdi + 1] && first_player * 2 + axis < 8)
							{
							   int player_num = first_player + axis / 2;
							   u16 x = mo_x_abs[player_num] * 0xFFFF / 639 + 0.5f;
							   u16 y = mo_y_abs[player_num] * 0xFFFF / 479 + 0.5f;
							   if (mo_x_abs[player_num] < 0 || mo_x_abs[player_num] > 639
									 || mo_y_abs[player_num] < 0 || mo_y_abs[player_num] > 479)
							   {
								   x = 0;
								   y = 0;
							   }
							   //ERROR_LOG(JVS, "P%d x,y:%4x,%4x ", player_num + 1, x, y);
							   JVS_OUT(x >> 8);		// X, MSB
							   JVS_OUT(x);			// X, LSB
							   axis++;
							   if (axis < buffer_in[cmdi + 1])
							   {
								  JVS_OUT(y >> 8);		// Y, MSB
								  JVS_OUT(y);			// Y, LSB
								  axis++;
							   }
							}
						}

						int axes_per_player = 4;
						if (naomi_game_inputs != NULL)
						{
						   int i = 0;
						   while (naomi_game_inputs->axes[i].name != NULL)
							  i++;
						   if (i == 0)
							  axes_per_player = 2;	// Default
						   else
							  axes_per_player = i;
						}
						if (settings.mapping.JammaSetup == JVS::DualIOBoards4P)
						   // Hack for Ring Out 4x4
						   axes_per_player = 2;

						for (; axis < buffer_in[cmdi + 1]; axis++)
						{
						   int player_num = first_player + axis / axes_per_player;
						   int player_axis = axis % axes_per_player;
						   u16 axis_value = 0x8000;
						   if (player_num < 4)
						   {
						   	bool inverted = false;
						   	if (naomi_game_inputs != NULL)
						   	{
						   		inverted = naomi_game_inputs->axes[player_axis].inverted;
						   		player_axis = naomi_game_inputs->axes[player_axis].axis;
						   	}
						   	axis_value = read_analog_axis(player_num, player_axis, inverted);
						   	if (settings.mapping.JammaSetup == JVS::Mazan && node_id == 2)
						   		// Hack for Mazan
						   		axis_value = axis_value;
						   }
						   LOGJVS("P%d.%d:%4x ", player_num + 1, player_axis + 1, axis_value);
						   JVS_OUT(axis_value >> 8);
						   JVS_OUT(axis_value);
						}
						cmdi += 2;
					}
					break;

				case 0x23:	// Read rotary encoders
					{
					   JVS_STATUS1();	// report byte
					   static f32 rotx[4] = { 0 };
					   static f32 roty[4] = { 0 };
					   LOGJVS("rotenc ");
					   for (int chan = 0; chan < buffer_in[cmdi + 1]; chan++)
					   {
						  u16 v;
						  if (chan & 1)
						  {
							 v = read_rotary_encoder(roty[chan / 2], -mo_y_delta[chan / 2] / 3.f);
							 mo_y_delta[chan / 2] = 0;
						  }
						  else
						  {
							 v = read_rotary_encoder(rotx[chan / 2], mo_x_delta[chan / 2] / 3.f);
							 mo_x_delta[chan / 2] = 0;
						  }
						  LOGJVS("%d:%4x ", chan, v);
						  JVS_OUT(v >> 8);		// MSB
						  JVS_OUT(v);			// LSB
					   }
					   cmdi += 2;
					}
					break;

				case 0x25:	// Read screen pos inputs
					{
						//ERROR_LOG(JVS, "Reading screen pos inputs");
						JVS_STATUS1();	// report byte
						u32 channel = buffer_in[cmdi + 1] - 1;
						int player_num = first_player + channel;
						if (player_num >= 4)
						{
							LOGJVS("P%d lightgun inv ", player_num + 1);
							JVS_OUT(0xFF);
							JVS_OUT(0xFF);
							JVS_OUT(0xFF);
							JVS_OUT(0xFF);
						}
						else
						{
						   //u16 x = mo_x_abs[player_num] * 0xFFFF / 639 + 0.5f;
						   //u16 y = (479 - mo_y_abs[player_num]) * 0xFFFF / 479 + 0.5f;
						   // Correct for Ninja Assault. Revisit for other games
						   u32 xr = 0x19d - 0x37;
						   u32 yr = 0x1fe - 0x40;
						   s16 x = mo_x_abs[player_num] * xr / 639 + 0x37;
						   s16 y = mo_y_abs[player_num] * yr / 479 + 0x40;
						   s16 origx = x;
						   s16 origy = y;

					   	   s16 xbuf = 8;
						   s16 ybuf = 40;
						   s16 xblock = 40;
						   s16 xblockoffset = 100;
						   s16 yblock = 60;
						   s16 yblockthresh = 160;
						   s16 yadjust = 40;
						   s16 ylowerthresh = 400;

						   bool debug = true;

						   // TO DO
						   // Lower overall sword position on screen, relative to mouse
						   // fix stabbing

						   if (settings.mapping.JammaSetup == JVS::Mazan && debug) {

						   	/*int xdelta = (node_id == 1) ? x1delta : x2delta;
						   	int ydelta = (node_id == 1) ? y1delta : y2delta;

						   	x = (mo_x_abs[player_num] + xdelta) * xr / 639 + 0x37;
						    y = (mo_y_abs[player_num] + ydelta) * yr / 479 + 0x40;

						    ERROR_LOG(JVS, "Sensor: %d - X: %d, Y: %d, xdelta: %d, ydelta: %d", 
						    	node_id, x, y, xdelta, ydelta);*/

						   	s16 xdelta = 0;
						   	s16 ydelta = 313 + (node_id-1) * 37;

						   	if (stabbing) {
						   		xdelta = 0;
						   		ydelta = 0;
						   	} 
						   	else if (parry)
						   	{
						   		xdelta = 334 + (node_id-1) * 76;
						   		if (x < 320) {
						   			xdelta = -xdelta;
						   		}
						   		ydelta = (s16)(-y * 54/480)+27;
						   	}

						   	ERROR_LOG(JVS, "Sensor: %d - X: %d, Y: %d, xdelta: %d, ydelta: %d, stab? %d, parry? %d", 
						    	node_id, x, y, xdelta, ydelta, stabbing, parry);

						   	x = (mo_x_abs[player_num] + xdelta) * xr / 639 + 0x37;
						    y = (mo_y_abs[player_num] + ydelta) * yr / 479 + 0x40;


						    //ERROR_LOG(JVS, "P%d lightgun %4x,%4x - Sensor: %d - XComputed: %u, Ycomputed: %u - Dx: %u, Dy: %u - Stab? %d", player_num + 1, x, y,
						   	//node_id, x, y, x-origx, y-origy, stabbing);
						   }


						   if (settings.mapping.JammaSetup == JVS::Mazan && !debug)
						   {
						      origy = (mo_y_abs[player_num]+yadjust) * yr / 479 + 0x40;	
						   	  if (node_id == 1 && !stabbing && ! parry) {

						   	  	if (mo_y_abs[player_num] < yblockthresh) {
							   		// block
							   		y = (yblockthresh/*mo_y_abs[player_num] + yblock*/) * yr / 479 + 0x40;
							   		if (mo_x_abs[player_num] <= 320)
							   			x = ((xblockoffset+mo_x_abs[player_num])) * xr / 639 + 0x37;
							   		else
							   			x = ((mo_x_abs[player_num]-xblockoffset)) * xr / 639 + 0x37;
						   	  	}
						   	  	else {
						   	  		// lower bounds
						   	  		if (mo_y_abs[player_num] + yadjust <= 479)
						   	  			y = (mo_y_abs[player_num] + yadjust) * yr / 479 + 0x40;
						   	  		else
						   	  			y = (479 - (479 - mo_y_abs[player_num] )/2) * yr / 479 + 0x40;
						   	  	}
						   	  }
						   	  if (node_id == 2 || stabbing || parry) {
							   	// node_id 2 is the sensor close to the player
							   	////// ATTEMPT 1:
							   	// Good, overall
							   	// low range of movement with the mouse - mouse needs to be stuck in the lower-center part of the screen
							   	/*
							   	if (mo_x_abs[player_num] > 320)
							   		x = (320 + ((mo_x_abs[player_num]-320)/2)) * xr / 639 + 0x37;
							   	else
							   		x = (320 - ((320-mo_x_abs[player_num])/2)) * xr / 639 + 0x37;
							   	y = (479 - ((479-mo_y_abs[player_num])/2)) * yr / 479 + 0x40;
							   	*/

							   	/*if (mo_x_abs[player_num] > 320)
							   		x = (320 + ((mo_x_abs[player_num]-320)/2)) * xr / 639 + 0x37;
							   	else
							   		x = (320 - ((320-mo_x_abs[player_num])/2)) * xr / 639 + 0x37;*/
							   	//y = (mo_y_abs[player_num] + 20) * yr / 479 + 0x40;

							   	
							   	////// ATTEMPT 2:
							   	// Adjust attempt 1 to mitigate problems
							   	// low range of movement with the mouse - mouse needs to be stuck in the lower-center part of the screen


							   	if (mo_y_abs[player_num] < yblockthresh) {
							   		// block
							   		y = (yblockthresh/*mo_y_abs[player_num] + yblock*/) * yr / 479 + 0x40;

							   		if (mo_x_abs[player_num] <= 320)
							   			x = ((xblockoffset+mo_x_abs[player_num]+xblock)) * xr / 639 + 0x37;
							   		else
							   			x = ((mo_x_abs[player_num]-xblockoffset-xblock)) * xr / 639 + 0x37;
							   	} 
							   	else
							   	{
							   		// middle of the screen, to center handle
								   	if (mo_x_abs[player_num] > 320)
								   		x = (320 + ((mo_x_abs[player_num]-320)/2)) * xr / 639 + 0x37;
								   	else
								   		x = (320 - ((320-mo_x_abs[player_num])/2)) * xr / 639 + 0x37;

								   	// avoid angle to be too steep or sword won't register
								    x = (x - origx > xbuf || x - origx < -xbuf) ? origx + xbuf * (((x - origx) > 0) - ((x - origx) < 0)): x;


								    if (mo_y_abs[player_num] <= ylowerthresh) {
								    	/*
								    	if (mo_y_abs[player_num] + yadjust <= ylowerthresh)
							   	  			y = (mo_y_abs[player_num] + yadjust) * yr / 479 + 0x40;
							   	  		else
							   	  			y = (ylowerthresh - (ylowerthresh -mo_y_abs[player_num] )/2) * yr / 479 + 0x40;
								    	*/

								    	if (mo_y_abs[player_num] <= ylowerthresh)
								    		y = ((ylowerthresh + yadjust - ((ylowerthresh-mo_y_abs[player_num]+yadjust)/2))) * yr / 479 + 0x40;
								    	
							   			y = (y - origy > ybuf || y - origy < -ybuf) ? origy + ybuf * (((y - origy) > 0) - ((y - origy) < 0)): y;
							   		}
							    }
						   	  }
						   }
						   /*ERROR_LOG(JVS, "P%d lightgun %4x,%4x - Sensor: %d - XComputed: %u, Ycomputed: %u - Dx: %u, Dy: %u - Stab? %d", player_num + 1, x, y,
						   	node_id, x, y, x-origx, y-origy, stabbing);*/
						   
						   JVS_OUT(x >> 8);		// X, MSB
						   JVS_OUT(x);			// X, LSB
						   JVS_OUT(y >> 8);		// Y, MSB
						   JVS_OUT(y);			// Y, LSB
						}
						cmdi += 2;
					}
					break;

				case 0x32:	// switched outputs
				case 0x33:
					LOGJVS("output(%d) %x", buffer_in[cmdi + 1], buffer_in[cmdi + 2]);
					write_digital_out(buffer_in[cmdi + 1], &buffer_in[cmdi + 2]);
					JVS_STATUS1();	// report byte
					cmdi += buffer_in[cmdi + 1] + 2;
					break;

				case 0x30:	// substract coin
					coin_count[buffer_in[cmdi + 1] - 1] -= (buffer_in[cmdi + 2] << 8) + buffer_in[cmdi + 3];
					JVS_STATUS1();	// report byte
					cmdi += 4;
					break;

				default:
					DEBUG_LOG(MAPLE, "JVS: Unknown input type %x", buffer_in[cmdi]);
					JVS_OUT(2);			// report byte: command error
					cmdi = length_in;	// Ignore subsequent commands
					break;
				}
			}
			LOGJVS("\n");
		}
		else
		{
			INFO_LOG(MAPLE, "JVS: Unknown JVS command %x", jvs_cmd);
			JVS_OUT(2);	// Unknown command
		}
		break;
	}
	jvs_length = length - 3;
	//ERROR_LOG(JVS, "FINISHED");
	return length;
}

bool jvs_io_board::maple_serialize(void **data, unsigned int *total_size)
{
   LIBRETRO_S(node_id);
   LIBRETRO_S(lightgun_as_analog);

   return true;
}

bool jvs_io_board::maple_unserialize(void **data, unsigned int *total_size)
{
   LIBRETRO_US(node_id);
   LIBRETRO_US(lightgun_as_analog);

   return true ;
}

maple_device* maple_Create(MapleDeviceType type)
{
	maple_device* rv=0;
	switch(type)
	{
	case MDT_SegaController:
		if (settings.System == DC_PLATFORM_ATOMISWAVE)
			rv = new maple_atomiswave_controller();
		else
			rv = new maple_sega_controller();
		break;

	case MDT_TwinStick:
		rv = new maple_sega_twinstick();
		break;

	case MDT_AsciiStick:
		rv = new maple_ascii_stick();
		break;

	case MDT_Microphone:
		rv = new maple_microphone();
		break;

	case MDT_SegaVMU:
		rv = new maple_sega_vmu();
		break;

   case MDT_PurupuruPack:
      rv = new maple_sega_purupuru();
      break;

   case MDT_Keyboard:
		rv = new maple_keyboard();
		break;

   case MDT_Mouse:
		rv = new maple_mouse();
		break;

   case MDT_LightGun:
		if (settings.System == DC_PLATFORM_ATOMISWAVE)
			rv = new atomiswave_lightgun();
		else
			rv = new maple_lightgun();
		break;

	case MDT_NaomiJamma:
		rv = new maple_naomi_jamma();
		break;

	default:
		ERROR_LOG(MAPLE, "Invalid device type %d", type);
		die("Invalid maple device type");
		break;
	}

	return rv;
}
