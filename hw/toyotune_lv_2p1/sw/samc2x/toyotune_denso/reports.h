#ifndef __REPORTS_H__
#define __REPORTS_H__

#ifdef GCC
#ifndef ATTR_PACKED
#define ATTR_PACKED __attribute__ ((packed))
#endif
#else
#ifndef ATTR_PACKED
#define ATTR_PACKED
#endif
#endif

#ifdef _MSC_VER
#pragma pack(push)
#pragma pack(1)
#endif

/* Maximum number of MCUs */
#define MCU_MAX (4)

#define DIAG_MEMORY_MAX_BYTES_SIZE	(64)
#define DIAG_FRAME_MAX_SIZE			(32)
#define MEMORY_MAX_SIZE				(64)

/* Number of bytes required to hold serial number */
#define SERIAL_NUMBER_SIZE (10)

#ifdef QT_CORE_LIB
typedef quint8  uint8_t;
typedef quint16 uint16_t;
typedef quint32 uint32_t;
#endif

enum
{
	DIAG_MEMORY_STATUS_DISABLED,
	DIAG_MEMORY_STATUS_IDLE,
	DIAG_MEMORY_STATUS_READ_PENDING,
	DIAG_MEMORY_STATUS_READ_FAILED,
	DIAG_MEMORY_STATUS_READ_OK,
	DIAG_MEMORY_STATUS_WRITE_PENDING,
	DIAG_MEMORY_STATUS_WRITE_FAILED,
	DIAG_MEMORY_STATUS_WRITE_OK,
};


/* MCU status input report */
#define HID_REPORT_ID_MCU_STATUS (0x01)
typedef struct
{
	struct
	{
		uint8_t Type:4;
		uint8_t IsRunning:1;
		uint8_t Reserved:3;
	} ATTR_PACKED MCU[MCU_MAX];
} ATTR_PACKED HID_Report_Mcu_Status_t;

/* Diagnostics status input report */
#define HID_REPORT_ID_DIAG_STATUS (0x02)
typedef struct
{
	uint8_t State:4;
	uint8_t Mode:2;
	uint8_t IsFrameError:1;
	uint8_t IsOverrunError:1;
	uint16_t FrameErrorCount;
	uint16_t OverrunErrorCount;
	struct
	{
		uint16_t SoftwareId;
	} ATTR_PACKED Diag[MCU_MAX];
} ATTR_PACKED HID_Report_Diag_Status_t;

/* Diagnostic results input report */
#define HID_REPORT_ID_DIAG (0x03)
typedef struct
{
    uint8_t Time;		// Time in milliseconds since last report, 0 if no previous report */
	uint8_t FrameId;
	uint8_t NumEntries;
	uint16_t Data[DIAG_FRAME_MAX_SIZE];
} ATTR_PACKED HID_Report_Diag_t;

/* Get flash address for active page(s) */
#define HID_REPORT_ID_FLASH_GET_PAGE_INFO (0x04)
typedef struct
{
	uint32_t Address[MCU_MAX];
	uint32_t Size[MCU_MAX];
} ATTR_PACKED HID_Report_Flash_Get_Page_Address_t;

/* Get SRAM address for active page(s) */
#define HID_REPORT_ID_SRAM_GET_PAGE_INFO (0x05)
typedef struct
{
	uint32_t Address[MCU_MAX];
	uint32_t Size[MCU_MAX];
} ATTR_PACKED HID_Report_SRAM_Get_Page_Address_t;

#define MEMORY_SELECT_MCU_FLASH			(0x00) /* Read/write to MCU Flash */
#define MEMORY_SELECT_MCU_SRAM			(0x01) /* Read/write to MCU SRAM */
#define MEMORY_SELECT_MCU_FLASH_SRAM	(0x02) /* Write to MCU Flash and SRAM together */
#define MEMORY_SELECT_MCU_METADATA		(0x04) /* Read/write to MCU meta-data */
#define MEMORY_SELECT_AVR_EEPROM		(0x05) /* Read/write to EEPROM */
#define MEMORY_SELECT_EXTERNAL_I2C		(0x06) /* Read/write to I2C peripheral */
#define MEMORY_SELECT_SPI_FLASH			(0x07) /* Read/write to SPI flash */

/* Memory set address output report */
#define HID_REPORT_ID_MEMORY_SET_ADDRESS (0x80)
typedef struct
{
	uint8_t  Select;		/* Select address space */
	uint8_t  Device;		/* Select MCU or I2C device */	
	uint32_t Address:26;	/* Memory address */
	uint32_t NumBytes:6;	/* 0 = MAX_SIZE bytes */
} ATTR_PACKED HID_Report_Memory_Set_Address_t;

/* Memory write output report */
#define HID_REPORT_ID_MEMORY_WRITE (0x81)
typedef struct
{
	uint8_t Data[MEMORY_MAX_SIZE];
} ATTR_PACKED HID_Report_Memory_Write_t;

/* Memory read input report */
#define HID_REPORT_ID_MEMORY_READ (0x81)
typedef struct
{
	uint8_t Data[MEMORY_MAX_SIZE];
} ATTR_PACKED HID_Report_Memory_Read_t;

#define CAP_MEMORY_READ		// Supports memory read
#define CAP_MEMORY_WRITE	// Supports memory write
#define CAP_DIAG			// Supports diagnostics
#define CAP_DIAG_STORE		// Supports local storage of diagnostic data
#define CAP_RT_MEMORY_READ	// Supports real-time memory read
#define CAP_RT_MEMORY_WRITE	// Supports real-time memory write
#define CAP_BT				// Supports Bluetooth

typedef struct
{
	uint32_t Capabilities;
	uint32_t McuCapabilities[MCU_MAX];
	uint8_t Reserved[40];
} Hardware_Capabilities_t;

/* Hardware About input report */
#define HID_REPORT_ID_ABOUT (0x85)
typedef struct
{
	uint32_t SoftwareVersionMajor;
	uint16_t SoftwareVersionMinor;
	uint16_t HardwareType;
	uint16_t HardwareCpldVersion;
    uint8_t HardwareSerialNum[SERIAL_NUMBER_SIZE];
	Hardware_Capabilities_t Capabilities;
	uint32_t EcuMask;
} ATTR_PACKED HID_Report_About_t;

/* Real-time memory write output report */
#define HID_REPORT_ID_DIAG_MEMORY_WRITE (0x86)
typedef struct
{
	uint8_t Mcu;
	uint16_t Address;
	uint8_t NumBytes;
	uint8_t Data[DIAG_MEMORY_MAX_BYTES_SIZE];
} ATTR_PACKED HID_Report_Diag_Memory_Write_t;

/* Real-time memory write output report */
#define HID_REPORT_ID_DIAG_MEMORY_READ (0x87)
typedef struct
{
	uint8_t Mcu;
	uint16_t Address;
	uint8_t NumBytes;
} ATTR_PACKED HID_Report_Diag_Memory_Read_t;

/* RAM read or write status input report */
#define HID_REPORT_ID_DIAG_MEMORY_STATUS (0x88)
typedef struct
{
	uint8_t Status;
	uint8_t	NumBytes;
	uint8_t Data[DIAG_MEMORY_MAX_BYTES_SIZE];
} ATTR_PACKED HID_Report_Diag_Memory_Status_t;

#if 0
/* Flash label output report */
#define HID_REPORT_ID_FLASH_LABEL_ACCESS (0x89)
typedef struct
{
	uint8_t Read:1;
	uint8_t Mcu:2;
	uint8_t Page:5;
	uint8_t Data[FLASH_LABEL_SIZE];
} ATTR_PACKED HID_Report_Flash_Label_Access_t;

/* Flash label input report */
#define HID_REPORT_ID_FLASH_LABEL_READ (0x8A)
typedef struct
{
	uint8_t Read:1;
	uint8_t Mcu:2;
	uint8_t Page:5;
	uint8_t Data[FLASH_LABEL_SIZE];
} ATTR_PACKED HID_Report_Flash_Label_Read_t;
#endif


/* Enable/disable Diagnostic recording */
#define HID_REPORT_ID_DIAG_RECORD (0xFB)
typedef struct
{
	uint8_t Enable:1;
	uint8_t Loop:1;
	uint16_t SectorStart;
	uint16_t SectorEnd;
} ATTR_PACKED HID_Report_Diag_Record_t;

/* MCU control feature report */
#define HID_REPORT_ID_MCU_CONTROL (0xFC)
typedef struct
{
	struct
	{
		uint8_t SetPage:1;
		uint8_t ActivePage:7;
	} ATTR_PACKED MCU[MCU_MAX];
} ATTR_PACKED HID_Report_Mcu_Control_t;

/* Diagnostic control feature report */
#define HID_REPORT_ID_DIAG_CONTROL  (0xFD)
typedef struct
{
	uint8_t ResetCounters:1;
	uint8_t Reserved:7;
	uint8_t FrameId;
	uint8_t NumEntries;
	struct  
	{
		uint16_t Mcu:2;
		uint16_t Size:1;
		uint16_t Address:13;
	}  Address[DIAG_FRAME_MAX_SIZE];
} ATTR_PACKED HID_Report_Diag_Control_t;

/* Mode selection feature report */
#define HID_REPORT_ID_MODE_CONTROL (0xFE)
typedef struct
{
	uint8_t Mode;
} ATTR_PACKED HID_Report_Mode_Control_t;

/* Debug control feature report */
#define HID_REPORT_ID_DEBUG_CONTROL (0xFF)
typedef struct
{
	uint8_t Enable:1;
	uint8_t Level:2;
} ATTR_PACKED HID_Report_Debug_Control_t;

/* Debug logging input report */
#define HID_REPORT_ID_DEBUG	(0xFF) 
#define DEBUG_MAX_SIZE (64)
typedef struct
{
	uint8_t NumChars;
	uint8_t Data[DEBUG_MAX_SIZE];
} ATTR_PACKED HID_Report_Debug_t;

#define HID_REPORT_MAX_SIZE (128)
#define HID_REPORT_SIZE(t) (sizeof(t ## _t))

#endif
