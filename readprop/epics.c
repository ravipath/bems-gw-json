/**************************************************************************
*
* Copyright (C) 2006 Steve Karg <skarg@users.sourceforge.net>
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*********************************************************************/

/** @file epics/main.c  Command line tool to build a full VTS3 EPICS file,
 *                          including the heading information. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>       /* for time */
#include <errno.h>
#include <assert.h>
#include "config.h"
#include "bactext.h"
#include "iam.h"
#include "arf.h"
#include "tsm.h"
#include "address.h"
#include "bacdef.h"
#include "npdu.h"
#include "apdu.h"
#include "device.h"
#include "net.h"
#include "datalink.h"
#include "whois.h"
#include "rp.h"
#include "proplist.h"
/* some demo stuff needed */
#include "filename.h"
#include "handlers.h"
#include "client.h"
#include "txbuf.h"
#include "dlenv.h"
#include "keylist.h"
#include "bacepics.h"
#include "iot_device_obj.h"


/* (Doxygen note: The next two lines pull all the following Javadoc
 *  into the BACEPICS module.) */
/** @addtogroup BACEPICS
 * @{ */

/** This is the souped-up version of the program for generating EPICS files.
 * It now:
 * 1) Prepends the heading information (supported services, etc)
 * 2) Determines some basic device properties for the header.
 * 3) Postpends the tail information to complete the EPICS file.
 */

/* buffer used for receive */
static uint8_t Rx_Buf[MAX_MPDU] = { 0 };

/* target information converted from command line */
static uint32_t target_device_obj_ins = BACNET_MAX_INSTANCE;
static BACNET_ADDRESS Target_Address;
/* the invoke id is needed to filter incoming messages */
static uint8_t Request_Invoke_ID = 0;
/* loopback address to talk to myself */
/* = { 6, { 127, 0, 0, 1, 0xBA, 0xC0, 0 }, 0 }; */
#if defined(BACDL_BIP)
/* If set, use this as the source port. */
static uint16_t My_BIP_Port = 0;
#endif
static bool Provided_Targ_MAC = false;

/* any errors are picked up in main loop */
static bool Error_Detected = false;
static uint16_t Last_Error_Class = 0;
static uint16_t Last_Error_Code = 0;
/* Counts errors we couldn't get around */
static uint16_t Error_Count = 0;
/* Assume device can do RPM, to start */
static bool Has_RPM = true;
static EPICS_STATES myState = INITIAL_BINDING;

uint32_t dev_identifier = 0;



/* any valid RP or RPM data returned is put here */
/* Now using one structure for both RP and RPM data:
 * typedef struct BACnet_RP_Service_Data_t {
 *   bool new_data;
 *   BACNET_CONFIRMED_SERVICE_ACK_DATA service_data;
 *   BACNET_READ_PROPERTY_DATA data;
 *   } BACNET_RP_SERVICE_DATA;
 * static BACNET_RP_SERVICE_DATA Read_Property_Data;
 */

typedef struct BACnet_RPM_Service_Data_t {
    bool new_data;
    BACNET_CONFIRMED_SERVICE_ACK_DATA service_data;
    BACNET_READ_ACCESS_DATA *rpm_data;
} BACNET_RPM_SERVICE_DATA;
static BACNET_RPM_SERVICE_DATA Read_Property_Multiple_Data;

/* We get the length of the object list,
   and then get the objects one at a time */
static uint32_t Object_List_Length = 0;
static int32_t Object_List_Index = 0;
/* object that we are currently printing */
static OS_Keylist Object_List;

/* When we need to process an Object's properties one at a time,
 * then we build and use this list */
#define MAX_PROPS 128   /* Supersized so it always is big enough. */
static uint32_t Property_List_Length = 0;
static uint32_t Property_List_Index = 0;
static int32_t Property_List[MAX_PROPS + 2];

struct property_value_list_t {
    int32_t property_id;
    BACNET_APPLICATION_DATA_VALUE *value;
};
static struct property_value_list_t Property_Value_List[] = {
    {PROP_VENDOR_NAME, NULL},
    {PROP_MODEL_NAME, NULL},
    {PROP_MAX_APDU_LENGTH_ACCEPTED, NULL},
    {PROP_PROTOCOL_SERVICES_SUPPORTED, NULL},
    {PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED, NULL},
    {PROP_DESCRIPTION, NULL},
    {-1, NULL}
};

// static BACNET_APPLICATION_DATA_VALUE *object_property_value(
//     int32_t property_id)
// {
//     BACNET_APPLICATION_DATA_VALUE *value = NULL;
//     int32_t index = 0;

//     do {
//         if (Property_Value_List[index].property_id == property_id) {
//             value = Property_Value_List[index].value;
//             break;
//         }
//         index++;
//     } while (Property_Value_List[index].property_id != -1);

//     return value;
// }

/* When we have to walk through an array of things, like ObjectIDs or
 * Subordinate_Annotations, one RP call at a time, use these for indexing.
 */
static uint32_t Walked_List_Length = 0;
static uint32_t Walked_List_Index = 0;
/* TODO: Probably should have done this as additional EPICS_STATES */
static bool Using_Walked_List = false;
/* When requesting RP for BACNET_ARRAY_ALL of what we know can be a long
 * array, then set this true in case it aborts and we need Using_Walked_List */
static bool IsLongArray = false;
/* Show value instead of '?' */
static bool ShowValues = false;
/* read required and optional properties when RPM ALL does not work */
static bool Optional_Properties = false;

#ifdef IOT

IOT_DEVICE_OBJECT_D2C myiotdeviceobject = NULL;
IOT_PROPERTY_LIST myiotpropertylist, temp = NULL;

static bool device_flag = false;
static uint8_t dev_prop_count = 0;

char *bufferzone;// = "NULL\\0";
char *prop_value;
uint8_t worldcounter = 0;

#endif

#if !defined(PRINT_ERRORS)
#define PRINT_ERRORS 1
#endif

static void MyErrorHandler(
    BACNET_ADDRESS * src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code)
{
    if (address_match(&Target_Address, src) &&
        (invoke_id == Request_Invoke_ID)) {
#if PRINT_ERRORS
        if (ShowValues) {
            fprintf(stderr, "-- BACnet Error: %s: %s\n",
                bactext_error_class_name(error_class),
                bactext_error_code_name(error_code));
        }
#endif
        Error_Detected = true;
        Last_Error_Class = error_class;
        Last_Error_Code = error_code;
    }
}

void MyReadPropertyAckHandler(
    uint8_t * service_request,
    uint16_t service_len,
    BACNET_ADDRESS * src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA * service_data)
{
    int len = 0;
    BACNET_READ_ACCESS_DATA *rp_data;

    if (address_match(&Target_Address, src) &&
        (service_data->invoke_id == Request_Invoke_ID)) {
        rp_data = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
        if (rp_data) {
            len =
                rp_ack_fully_decode_service_request(service_request,
                service_len, rp_data);
        }
        if (len > 0) {
            memmove(&Read_Property_Multiple_Data.service_data, service_data,
                sizeof(BACNET_CONFIRMED_SERVICE_ACK_DATA));
            Read_Property_Multiple_Data.rpm_data = rp_data;
            Read_Property_Multiple_Data.new_data = true;
        } else {
            if (len < 0)        /* Eg, failed due to no segmentation */
                Error_Detected = true;
            free(rp_data);
        }
    }
}

void MyReadPropertyMultipleAckHandler(
    uint8_t * service_request,
    uint16_t service_len,
    BACNET_ADDRESS * src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA * service_data)
{
    int len = 0;
    BACNET_READ_ACCESS_DATA *rpm_data;

    if (address_match(&Target_Address, src) &&
        (service_data->invoke_id == Request_Invoke_ID)) {
        rpm_data = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
        if (rpm_data) {
            len =
                rpm_ack_decode_service_request(service_request, service_len,
                rpm_data);
        }
        if (len > 0) {
            memmove(&Read_Property_Multiple_Data.service_data, service_data,
                sizeof(BACNET_CONFIRMED_SERVICE_ACK_DATA));
            Read_Property_Multiple_Data.rpm_data = rpm_data;
            Read_Property_Multiple_Data.new_data = true;
            /* Will process and free the RPM data later */
        } else {
            if (len < 0)        /* Eg, failed due to no segmentation */
                Error_Detected = true;
            free(rpm_data);
        }
    }
}


static void Init_Service_Handlers(
     void)
 {
//     Device_Init(NULL);

 #if BAC_ROUTING
     uint32_t Object_Instance;
     BACNET_CHARACTER_STRING name_string;
     /* Put this client Device into the Routing table (first entry) */
    Object_Instance = Device_Object_Instance_Number();
     Device_Object_Name(Object_Instance, &name_string);
     Add_Routed_Device(Object_Instance, &name_string, Device_Description());
 #endif

//     /* we need to handle who-is
//        to support dynamic device binding to us */
    //  apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
//     /* handle i-am to support binding to other devices */
      apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
//     /* set the handler for all the services we don't implement
//        It is required to send the proper reject message... */
    //  apdu_set_unrecognized_service_handler_handler
    //      (handler_unrecognized_service);
//     /* we must implement read property - it's required! */
    //  apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,
    //      handler_read_property);
//     /* handle the data coming back from confirmed requests */
     apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROPERTY,
         MyReadPropertyAckHandler);
     apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
         MyReadPropertyMultipleAckHandler);
//     /* handle any errors coming back */
     apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROPERTY, MyErrorHandler);
//     // apdu_set_abort_handler(MyAbortHandler);
//     // apdu_set_reject_handler(MyRejectHandler);
}
/** Determine if this is a writable property, and, if so,
 * note that in the EPICS output.
 * This function may need a lot of customization for different implementations.
 *
 * @param object_type [in] The BACnet Object type of this object.
 * @note  object_instance [in] The ID number for this object.
 * @param rpm_property [in] Points to structure holding the Property,
 *                          Value, and Error information.
 */
void CheckIsWritableProperty(
    BACNET_OBJECT_TYPE object_type,
    /* uint32_t object_instance, */
    BACNET_PROPERTY_REFERENCE * rpm_property)
{
    bool bIsWritable = false;
    if ((object_type == OBJECT_ANALOG_OUTPUT) ||
        (object_type == OBJECT_BINARY_OUTPUT) ||
        (object_type == OBJECT_COMMAND) ||
        (object_type == OBJECT_MULTI_STATE_OUTPUT) ||
        (object_type == OBJECT_ACCESS_DOOR)) {
        if (rpm_property->propertyIdentifier == PROP_PRESENT_VALUE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_AVERAGING) {
        if ((rpm_property->propertyIdentifier == PROP_ATTEMPTED_SAMPLES) ||
            (rpm_property->propertyIdentifier == PROP_WINDOW_INTERVAL) ||
            (rpm_property->propertyIdentifier == PROP_WINDOW_SAMPLES)) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_FILE) {
        if (rpm_property->propertyIdentifier == PROP_ARCHIVE) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_LIFE_SAFETY_POINT) ||
        (object_type == OBJECT_LIFE_SAFETY_ZONE)) {
        if (rpm_property->propertyIdentifier == PROP_MODE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_PROGRAM) {
        if (rpm_property->propertyIdentifier == PROP_PROGRAM_CHANGE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_PULSE_CONVERTER) {
        if (rpm_property->propertyIdentifier == PROP_ADJUST_VALUE) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_TRENDLOG) ||
        (object_type == OBJECT_EVENT_LOG) ||
        (object_type == OBJECT_TREND_LOG_MULTIPLE)) {
        if ((rpm_property->propertyIdentifier == PROP_ENABLE) ||
            (rpm_property->propertyIdentifier == PROP_RECORD_COUNT)) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_LOAD_CONTROL) {
        if ((rpm_property->propertyIdentifier == PROP_REQUESTED_SHED_LEVEL) ||
            (rpm_property->propertyIdentifier == PROP_START_TIME) ||
            (rpm_property->propertyIdentifier == PROP_SHED_DURATION) ||
            (rpm_property->propertyIdentifier == PROP_DUTY_WINDOW) ||
            (rpm_property->propertyIdentifier == PROP_SHED_LEVELS)) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_ACCESS_ZONE) ||
        (object_type == OBJECT_ACCESS_USER) ||
        (object_type == OBJECT_ACCESS_RIGHTS) ||
        (object_type == OBJECT_ACCESS_CREDENTIAL)) {
        if (rpm_property->propertyIdentifier == PROP_GLOBAL_IDENTIFIER) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_NETWORK_SECURITY) {
        if ((rpm_property->propertyIdentifier ==
                PROP_BASE_DEVICE_SECURITY_POLICY) ||
            (rpm_property->propertyIdentifier ==
                PROP_NETWORK_ACCESS_SECURITY_POLICIES) ||
            (rpm_property->propertyIdentifier == PROP_SECURITY_TIME_WINDOW) ||
            (rpm_property->propertyIdentifier == PROP_PACKET_REORDER_TIME) ||
            (rpm_property->propertyIdentifier == PROP_LAST_KEY_SERVER) ||
            (rpm_property->propertyIdentifier == PROP_SECURITY_PDU_TIMEOUT) ||
            (rpm_property->propertyIdentifier == PROP_DO_NOT_HIDE)) {
            bIsWritable = true;
        }
    }
    /* Add more checking here, eg for Time_Synchronization_Recipients,
     * Manual_Slave_Address_Binding, Object_Property_Reference,
     * Life Safety Tracking_Value, Reliability, Mode,
     * or Present_Value when Out_Of_Service is TRUE.
     */
    if (bIsWritable)
        fprintf(stdout, " Writable");
}


static const char *protocol_services_supported_text(
    size_t bit_index)
{
    bool is_confirmed = false;
    size_t text_index = 0;
    bool found = false;
    const char *services_text = "unknown";

    found =
        apdu_service_supported_to_index(bit_index, &text_index, &is_confirmed);
    if (found) {
        if (is_confirmed) {
            services_text = bactext_confirmed_service_name(text_index);
        } else {
            services_text = bactext_unconfirmed_service_name(text_index);
        }
    }

    return services_text;
}

/** Provide a nicer output for Supported Services and Object Types bitfields
 * and Date fields.
 * We have to override the library's normal bitfield print because the
 * EPICS format wants just T and F, and we want to provide (as comments)
 * the names of the active types.
 * These bitfields use opening and closing parentheses instead of braces.
 * We also limit the output to 4 bit fields per line.
 *
 * @param stream [in] Normally stdout
 * @param object_value [in] The structure holding this property's description
 *                          and value.
 * @return True if success.  Or otherwise.
 */

bool PrettyPrintPropertyValue(
    FILE * stream,
    BACNET_OBJECT_PROPERTY_VALUE * object_value)
{
    BACNET_APPLICATION_DATA_VALUE *value = NULL;
    bool status = true; /*return value */
    size_t len = 0, i = 0, j = 0;
    BACNET_PROPERTY_ID property = PROP_ALL;
    char short_month[4];

    value = object_value->value;
    property = object_value->object_property;
    if ((value != NULL) && (value->tag == BACNET_APPLICATION_TAG_BIT_STRING) &&
        ((property == PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED) ||
            (property == PROP_PROTOCOL_SERVICES_SUPPORTED))) {
        len = bitstring_bits_used(&value->type.Bit_String);
        fprintf(stream, "( \n        ");
        for (i = 0; i < len; i++) {
            fprintf(stream, "%s", bitstring_bit(&value->type.Bit_String,
                    (uint8_t) i) ? "T" : "F");
            if (i < len - 1)
                fprintf(stream, ",");
            else
                fprintf(stream, " ");
            /* Tried with 8 per line, but with the comments, got way too long. */
            if ((i == (len - 1)) || ((i % 4) == 3)) {   /* line break every 4 */
                fprintf(stream, "   -- ");      /* EPICS comments begin with "--" */
                /* Now rerun the same 4 bits, but print labels for true ones */
                for (j = i - (i % 4); j <= i; j++) {
                    if (bitstring_bit(&value->type.Bit_String, (uint8_t) j)) {
                        if (property == PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED) {
                            fprintf(stream, " %s,",
                                bactext_object_type_name(j));
                        } else {
                            /* PROP_PROTOCOL_SERVICES_SUPPORTED */
                            fprintf(stream, " %s,",
                                protocol_services_supported_text(j));
                        }
                    } else      /* not supported */
                        fprintf(stream, ",");
                }
                fprintf(stream, "\n        ");
            }
        }
        fprintf(stream, ") \n");
    } else if ((value != NULL) && (value->tag == BACNET_APPLICATION_TAG_DATE)) {
        /* eg, property == PROP_LOCAL_DATE
         * VTS needs (3-Aug-2011,4) or (8/3/11,4), so we'll use the
         * clearer, international form. */
        strncpy(short_month, bactext_month_name(value->type.Date.month), 3);
        short_month[3] = 0;
        fprintf(stream, "(%u-%3s-%u, %u)", (unsigned) value->type.Date.day,
            short_month, (unsigned) value->type.Date.year,
            (unsigned) value->type.Date.wday);
    } else if (value != NULL) {
        assert(false);  /* How did I get here?  Fix your code. */
        /* Meanwhile, a fallback plan */
        status = bacapp_print_value(stdout, object_value);
    } else
        fprintf(stream, "? \n");

    return status;
}

#ifdef IOT



/*IOT_PROPERTY_LIST*/ void get_prop_object(/*IOT_DEVICE_OBJECT_D2C dev, char *str*/){

    IOT_PROPERTY_LIST temp, temp1;

    if(myiotdeviceobject == NULL){
        printf("if you see this, something is wrong. It's not possible to reach here. Fix your code\n");
        return;
    }
   // printf("get_prop_object\n");
    temp = myiotdeviceobject->iot_prop_list;//myiotdeviceobject->iot_prop_list;

    if(temp == NULL){

        temp = (IOT_PROPERTY_LIST) malloc(sizeof(struct iot_property_list));
        temp->next = NULL;
        temp->isempty = 1;
        /*myiotpropertylist*/
        myiotdeviceobject->iot_prop_list = temp;
        //myiotdeviceobject->iot_prop_list = myiotpropertylist;
        //printf("get_prop_object: temp was null\n");
    }
    else{

        while(temp->next != NULL){
            temp = temp->next;
            //printf("found an object in the list\n");
        }
        if(!temp->isempty){
            temp1 = (IOT_PROPERTY_LIST) malloc(sizeof(struct iot_property_list));
            temp1->next = NULL;
            temp1->isempty = 1;
            temp->next = temp1;
        }
        //printf("get_prop_object: temp was NOT null\n");
    }

}
#endif


/** Print out the value(s) for one Property.
 * This function may be called repeatedly for one property if we are walking
 * through a list (Using_Walked_List is True) to show just one value of the
 * array per call.
 *
 * @param object_type [in] The BACnet Object type of this object.
 * @param object_instance [in] The ID number for this object.
 * @param rpm_property [in] Points to structure holding the Property,
 *                          Value, and Error information.
 */
void PrintReadPropertyData(
    BACNET_OBJECT_TYPE object_type,
    uint32_t object_instance,
    BACNET_PROPERTY_REFERENCE * rpm_property)
{
    BACNET_OBJECT_PROPERTY_VALUE object_value;  /* for bacapp printing */
    BACNET_APPLICATION_DATA_VALUE *value, *old_value;
    bool print_brace = false;
    KEY object_list_element;
    bool isSequence = false;    /* Ie, will need bracketing braces {} */

#ifdef IOT
uint8_t ch;
//char pvalue;
char typebuf[10];
char instancebuf[10];
char ts_buffer[50];
IOT_PROPERTY_LIST temp = NULL;//, temp1;
#endif

    if (rpm_property == NULL) {
        fprintf(stdout, "    -- Null Property data \n");
        return;
    }
    value = rpm_property->value;
    if (value == NULL) {
        /* Then we print the error information */
        fprintf(stdout, "?  -- BACnet Error: %s: %s\n",
            bactext_error_class_name((int) rpm_property->error.error_class),
            bactext_error_code_name((int) rpm_property->error.error_code));
        return;
    }
    object_value.object_type = object_type;
    object_value.object_instance = object_instance;
    if ((value != NULL) && (value->next != NULL)) {
        /* Then this is an array of values.
         * But are we showing Values?  We (VTS3) want ? instead of {?,?} to show up. */
        switch (rpm_property->propertyIdentifier) {
                /* Screen the Properties that can be arrays or Sequences */
            case PROP_PRESENT_VALUE:
            case PROP_PRIORITY_ARRAY:
                if (!ShowValues) {
                    /* We want the Values freed below, but don't want to
                     * print anything for them.  To achieve this, swap
                     * out the Property for a non-existent Property
                     * and catch that below.  */
                    rpm_property->propertyIdentifier =
                        PROP_PROTOCOL_CONFORMANCE_CLASS;
                    break;
                }
                if (object_type == OBJECT_DATETIME_VALUE)
                    break;      /* A special case - no braces for this pair */
                /* Else, fall through to normal processing. */
            default:
                /* Normal array: open brace */
                fprintf(stdout, "{ ");
                print_brace = true;     /* remember to close it */
                break;
        }
    }

    if (!Using_Walked_List)
        Walked_List_Index = Walked_List_Length = 0;     /* In case we need this. */
    /* value(s) loop until there is no "next" ... */
    while (value != NULL) {
        object_value.object_property = rpm_property->propertyIdentifier;
        object_value.array_index = rpm_property->propertyArrayIndex;
        object_value.value = value;
        switch (rpm_property->propertyIdentifier) {
                /* These are all arrays, so they open and close with braces */
            case PROP_OBJECT_LIST:
            case PROP_STATE_TEXT:
            case PROP_STRUCTURED_OBJECT_LIST:
            case PROP_SUBORDINATE_ANNOTATIONS:
            case PROP_PROTOCOL_CONFORMANCE_CLASS:
            case PROP_SUBORDINATE_LIST:
                if (Using_Walked_List) {
                    if ((rpm_property->propertyArrayIndex == 0) &&
                        (value->tag == BACNET_APPLICATION_TAG_UNSIGNED_INT)) {
                        /* Grab the value of the Object List length - don't print it! */
                        Walked_List_Length = value->type.Unsigned_Int;
                        if (rpm_property->propertyIdentifier ==
                            PROP_OBJECT_LIST)
                            Object_List_Length = value->type.Unsigned_Int;
                        break;
                    } else
                        assert(Walked_List_Index == (uint32_t)
                            rpm_property->propertyArrayIndex);
                } else {
                    Walked_List_Index++;
                    /* If we got the whole Object List array in one RP call, keep
                     * the Index and List_Length in sync as we cycle through. */
                    if (rpm_property->propertyIdentifier == PROP_OBJECT_LIST)
                        Object_List_Length = ++Object_List_Index;
                }
                if (Walked_List_Index == 1) {
                    /* If the array is empty (nothing for this first entry),
                     * Make it VTS3-friendly and don't show "Null" as a value. */
                    if (value->tag == BACNET_APPLICATION_TAG_NULL && (rpm_property->propertyIdentifier != PROP_PROTOCOL_CONFORMANCE_CLASS)) {
                        //printf("rpm_property->propertyIdentifier = %d\n", rpm_property->propertyIdentifier);
                        fprintf(stdout, "?\n        ");
                        break;
                    }
                    else if (value->tag == BACNET_APPLICATION_TAG_NULL && (rpm_property->propertyIdentifier == PROP_PROTOCOL_CONFORMANCE_CLASS)) {
                        fprintf(stdout, "{");
                        break;
                    }
                    /* Open this Array of Objects for the first entry (unless
                     * opening brace has already printed, since this is an array
                     * of values[] ) */
                    if (value->next == NULL)
                        fprintf(stdout, "{ \n        ");
                    else
                        fprintf(stdout, "\n        ");
                }

                if (rpm_property->propertyIdentifier == PROP_OBJECT_LIST) {
                    if (value->tag != BACNET_APPLICATION_TAG_OBJECT_ID) {
                        assert(value->tag == BACNET_APPLICATION_TAG_OBJECT_ID); /* Something not right here */
                        break;
                    }
                    /* Store the object list so we can interrogate
                       each object. */
                    object_list_element =
                        KEY_ENCODE(value->type.Object_Id.type,
                        value->type.Object_Id.instance);
                    /* We don't have anything to put in the data pointer
                     * yet, so just leave it null.  The key is Key here. */
                    Keylist_Data_Add(Object_List, object_list_element, NULL);
                } else if (rpm_property->propertyIdentifier == PROP_STATE_TEXT) {
                    /* Make sure it fits within 31 chars for original VTS3 limitation.
                     * If longer, take first 15 dash, and last 15 chars. */
                    if (value->type.Character_String.length > 31) {
                        int iLast15idx =
                            value->type.Character_String.length - 15;
                        value->type.Character_String.value[15] = '-';
                        memcpy(&value->type.Character_String.value[16],
                            &value->type.Character_String.value[iLast15idx],
                            15);
                        value->type.Character_String.value[31] = 0;
                        value->type.Character_String.length = 31;
                    }
                } else if (rpm_property->propertyIdentifier ==
                    PROP_SUBORDINATE_LIST) {
                    if (value->tag != BACNET_APPLICATION_TAG_OBJECT_ID) {
                        assert(value->tag == BACNET_APPLICATION_TAG_OBJECT_ID); /* Something not right here */
                        break;
                    }
                    /* TODO: handle Sequence of { Device ObjID, Object ID }, */
                    isSequence = true;
                }

                /* If the object is a Sequence, it needs its own bracketing braces */
                if (isSequence)
                    fprintf(stdout, "{");
                bacapp_print_value(stdout, &object_value);
                if (isSequence)
                    fprintf(stdout, "}");

                if ((Walked_List_Index < Walked_List_Length) ||
                    (value->next != NULL)) {
                    /* There are more. */
                    fprintf(stdout, ", ");
                    if (!(Walked_List_Index % 4)){
                        if(rpm_property->propertyIdentifier != PROP_PROTOCOL_CONFORMANCE_CLASS)
                            fprintf(stdout, "\n        ");
                    }
                } else {
                    fprintf(stdout, " } \n");
                }
                break;

            case PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED:
            case PROP_PROTOCOL_SERVICES_SUPPORTED:
                PrettyPrintPropertyValue(stdout, &object_value);
                break;

                /* Our special non-existent case; do nothing further here. */
            // case PROP_PROTOCOL_CONFORMANCE_CLASS:
            //     break;

            default:
                /* First, if this is a date type, it needs a different format
                 * for VTS, so pretty print it. */
                if (ShowValues &&
                    (object_value.value->tag == BACNET_APPLICATION_TAG_DATE)) {
                    /* This would be PROP_LOCAL_DATE, or OBJECT_DATETIME_VALUE,
                     * or OBJECT_DATE_VALUE                     */
                    PrettyPrintPropertyValue(stdout, &object_value);
                } else {
                    /* Some properties are presented just as '?' in an EPICS;
                     * screen these out here, unless ShowValues is true.  */
                    switch (rpm_property->propertyIdentifier) {
                        case PROP_DEVICE_ADDRESS_BINDING:
                            /* Make it VTS3-friendly and don't show "Null"
                             * as a value. */
                            if (value->tag == BACNET_APPLICATION_TAG_NULL) {
                                fprintf(stdout, "?");
                                break;
                            }
                            /* Else, fall through for normal processing. */
                        case PROP_DAYLIGHT_SAVINGS_STATUS:
                        case PROP_LOCAL_TIME:
                        case PROP_LOCAL_DATE:  /* Only if !ShowValues */
                        #ifdef IOT
                        //case PROP_PRESENT_VALUE: /*handle this also in the default case as we need this for our property list*/
                        #else
                        case PROP_PRESENT_VALUE:
                        #endif
                        case PROP_PRIORITY_ARRAY:
                        case PROP_RELIABILITY:
                        case PROP_UTC_OFFSET:
                        case PROP_DATABASE_REVISION:
                            if (!ShowValues) {
                                bacapp_print_value(stdout, &object_value);
                                break;
                            }
                            /* Else, fall through and print value: */
                        default:
#ifdef IOT
                            ch = rpm_property->propertyIdentifier;
                            if(ch == PROP_OBJECT_IDENTIFIER || ch == PROP_OBJECT_TYPE || ch == PROP_MODEL_NAME || ch == PROP_PRESENT_VALUE 
                            || ch == PROP_SEGMENTATION_SUPPORTED){

                                if(ch == PROP_OBJECT_IDENTIFIER || ch == PROP_PRESENT_VALUE){
                                    temp = myiotdeviceobject->iot_prop_list;
                                    if(temp == NULL) printf(" -- List is null. fix your code buddy -- ");
                                    while(temp->next != NULL)
                                        temp = temp->next;
                                temp->isempty = 0;
                                memset(ts_buffer, 0, 50);
                                strcpy(temp->ts_lastchanged, get_ts_string(ts_buffer));
                                }

                              switch(rpm_property->propertyIdentifier){

                                case PROP_OBJECT_IDENTIFIER:
                                    temp->obj_prop_type = object_value.value->type.Object_Id.type;
                                    temp->obj_prop_instance = object_value.value->type.Object_Id.instance;
                                    sprintf(typebuf,"%d",object_value.value->type.Object_Id.type);
                                    sprintf(instancebuf,"%d",object_value.value->type.Object_Id.instance);
                                    strcat(typebuf,":");
                                    strcat(typebuf,instancebuf);
                                    strcpy(temp->prop_instance, typebuf);
                                break;
                                case PROP_OBJECT_TYPE:
                                if(strcmp(iot_print_value(&object_value), "device") == 0){
                                    device_flag = true;
                                }
                                break;
                                 case PROP_MODEL_NAME://save the description only of the device
                                    if(device_flag){
                                        myiotdeviceobject->description = iot_print_value(&object_value);
                                        //device_flag = true;
                                        dev_prop_count++;
                                        if(dev_prop_count == 2)
                                            device_flag = false;
                                    }
                                    
                                 break;
                                case PROP_PRESENT_VALUE:
                                    strcpy(temp->present_value,iot_print_value(&object_value));
                                break;
                                case PROP_SEGMENTATION_SUPPORTED:
                                if(device_flag){
                                        myiotdeviceobject->segmt = iot_print_value(&object_value);
                                        dev_prop_count++;
                                        if(dev_prop_count == 2)
                                            device_flag = false;
                                    }
                                break;
                                default:
                                    break;
                            }
                            }
#endif
                            bacapp_print_value(stdout, &object_value);
                            break;
                    }
                }
                if (value->next != NULL) {
                    /* there's more! */
                    fprintf(stdout, ",");
                } else {
                    if (print_brace) {
                        /* Closing brace for this multi-valued array */
                        fprintf(stdout, " }");
                    }
                    CheckIsWritableProperty(object_type,        /* object_instance, */
                        rpm_property);
                    fprintf(stdout, "\n");
                }
                break;
        }

        old_value = value;
        value = value->next;    /* next or NULL */
        free(old_value);
    }   /* End while loop */

}

/** Print the property identifier name to stdout,
 *  handling the proprietary property numbers.
 * @param propertyIdentifier [in] The property identifier number.
 */
static void Print_Property_Identifier(
    unsigned propertyIdentifier)
{
    if (propertyIdentifier < 512) {
        fprintf(stdout, "%s", bactext_property_name(propertyIdentifier));
    } else {
        fprintf(stdout, "-- proprietary %u", propertyIdentifier);
    }
}

/* Build a list of device properties to request with RPM. */
static void BuildPropRequest(
    BACNET_READ_ACCESS_DATA * rpm_object)
{
    int i;
    /* To start with, StartNextObject() has prepopulated one propEntry,
     * but we will overwrite it and link more to it
     */
    BACNET_PROPERTY_REFERENCE *propEntry = rpm_object->listOfProperties;
    BACNET_PROPERTY_REFERENCE *oldEntry = rpm_object->listOfProperties;
    for (i = 0; Property_Value_List[i].property_id != -1; i++) {
        if (propEntry == NULL) {
            propEntry = calloc(1, sizeof(BACNET_PROPERTY_REFERENCE));
            assert(propEntry);
            oldEntry->next = propEntry;
        }
        propEntry->propertyIdentifier = Property_Value_List[i].property_id;
        propEntry->propertyArrayIndex = BACNET_ARRAY_ALL;
        propEntry->next = NULL;
        oldEntry = propEntry;
        propEntry = NULL;
    }
}

/** Send an RP request to read one property from the current Object.
 * Singly process large arrays too, like the Device Object's Object_List.
 * If GET_LIST_OF_ALL_RESPONSE failed, we will fall back to using just
 * the list of known Required properties for this type of object.
 *
 * @param device_instance [in] Our target device's instance.
 * @param pMyObject [in] The current Object's type and instance numbers.
 * @return The invokeID of the message sent, or 0 if reached the end
 *         of the property list.
 */
static uint8_t Read_Properties(
    uint32_t device_instance,
    BACNET_OBJECT_ID * pMyObject)
{
    uint8_t invoke_id = 0;
    struct special_property_list_t PropertyListStruct;
    unsigned int i = 0, j = 0;

    if ((!Has_RPM && (Property_List_Index == 0)) ||
        (Property_List_Length == 0)) {
        /* If we failed to get the Properties with RPM, just settle for what we
         * know is the fixed list of Required and Optional properties.
         * In practice, this should only happen for simple devices that don't
         * implement RPM or have really limited MAX_APDU size.
         */
        property_list_special(pMyObject->type, &PropertyListStruct);
        if (Optional_Properties) {
            Property_List_Length =
                PropertyListStruct.Required.count +
                PropertyListStruct.Optional.count;
        } else {
            Property_List_Length = PropertyListStruct.Required.count;
        }
        if (Property_List_Length > MAX_PROPS) {
            Property_List_Length = MAX_PROPS;
        }
        /* Copy this list for later one-by-one processing */
        for (i = 0; i < Property_List_Length; i++) {
            if (i < PropertyListStruct.Required.count) {
                Property_List[i] = PropertyListStruct.Required.pList[i];
            } else if (Optional_Properties) {
                Property_List[i] = PropertyListStruct.Optional.pList[j];
                j++;
            }
        }
        /* Just to be sure we terminate */
        Property_List[i] = -1;
    }
    //printf("Property_List_Index = %d\n", Property_List_Index);
    if (Property_List[Property_List_Index] != -1) {
        int prop = Property_List[Property_List_Index];
        uint32_t array_index;
        IsLongArray = false;
        if (Using_Walked_List) {
            if (Walked_List_Length == 0) {
                array_index = 0;
            } else {
                array_index = Walked_List_Index;
            }
        } else {
            fprintf(stdout, "    ");
            Print_Property_Identifier(prop);
            fprintf(stdout, ": ");
            array_index = BACNET_ARRAY_ALL;

            switch (prop) {
                    /* These are all potentially long arrays, so they may abort */
                case PROP_OBJECT_LIST:
                case PROP_STATE_TEXT:
                case PROP_STRUCTURED_OBJECT_LIST:
                case PROP_SUBORDINATE_ANNOTATIONS:
                case PROP_SUBORDINATE_LIST:
                    IsLongArray = true;
                    break;
            }
        }

        invoke_id =
            Send_Read_Property_Request(device_instance, pMyObject->type,
            pMyObject->instance, prop, array_index);
            //printf("invoke_id = %d\n", invoke_id);

    }

    return invoke_id;
}

/** Process the RPM list, either printing out on success or building a
 *  properties list for later use.
 *  Also need to free the data in the list.
 *  If the present state is GET_HEADING_RESPONSE, store the results
 *  in globals for later use.
 * @param rpm_data [in] The list of RPM data received.
 * @param myState [in] The current state.
 * @return The next state of the EPICS state machine, normally NEXT_OBJECT
 *         if the RPM got good data, or GET_PROPERTY_REQUEST if we have to
 *         singly process the list of Properties.
 */
EPICS_STATES ProcessRPMData(
    BACNET_READ_ACCESS_DATA * rpm_data,
    EPICS_STATES myState)
{
    BACNET_READ_ACCESS_DATA *old_rpm_data;
    BACNET_PROPERTY_REFERENCE *rpm_property;
    BACNET_PROPERTY_REFERENCE *old_rpm_property;
    BACNET_APPLICATION_DATA_VALUE *value;
    BACNET_APPLICATION_DATA_VALUE *old_value;
    bool bSuccess = true;
    EPICS_STATES nextState = myState;   /* assume no change */
    /* Some flags to keep the output "pretty" -
     * wait and put these object lists at the end */
    bool bHasObjectList = false;
    bool bHasStructuredViewList = false;
    int i = 0;

    while (rpm_data) {
        rpm_property = rpm_data->listOfProperties;
        while (rpm_property) {
            /* For the GET_LIST_OF_ALL_RESPONSE case,
             * just keep what property this was */
            if (myState == GET_LIST_OF_ALL_RESPONSE) {
                //printf("Call ProcessRPMData:: myState == GET_LIST_OF_ALL_RESPONSE\n");
                switch (rpm_property->propertyIdentifier) {
                    case PROP_OBJECT_LIST:
                        bHasObjectList = true;  /* Will append below */
                        break;
                    case PROP_STRUCTURED_OBJECT_LIST:
                        bHasStructuredViewList = true;
                        break;
                    default:
                        Property_List[Property_List_Index] =
                            rpm_property->propertyIdentifier;
                        Property_List_Index++;
                        Property_List_Length++;
                        break;
                }
                /* Free up the value(s) */
                value = rpm_property->value;
                while (value) {
                    old_value = value;
                    value = value->next;
                    free(old_value);
                }
            } else if (myState == GET_HEADING_RESPONSE) {
               // printf("Call ProcessRPMData:: myState == GET_HEADING_RESPONSE\n");
                Property_Value_List[i++].value = rpm_property->value;
                /* copy this pointer.
                 * On error, the pointer will be null
                 * We won't free these values; they will free at exit */
            } else {
               // printf("Call ProcessRPMData:: ELSE because it doesnt matches anything else\n");
                fprintf(stdout, "    ");
                Print_Property_Identifier(rpm_property->propertyIdentifier);
                //fprintf(stdout, ":<ravi2> ");
                fprintf(stdout, ": ");
                PrintReadPropertyData(rpm_data->object_type,
                    rpm_data->object_instance, rpm_property);
            }
            old_rpm_property = rpm_property;
            rpm_property = rpm_property->next;
            free(old_rpm_property);
        }
        old_rpm_data = rpm_data;
        rpm_data = rpm_data->next;
        free(old_rpm_data);
    }

    /* Now determine the next state */
    if (myState == GET_HEADING_RESPONSE)
        nextState = PRINT_HEADING;
    /* press ahead with or without the data */
    else if (bSuccess && (myState == GET_ALL_RESPONSE))
        nextState = NEXT_OBJECT;
    else if (bSuccess) {        /* and GET_LIST_OF_ALL_RESPONSE */
        /* Now append the properties we waited on. */
        if (bHasStructuredViewList) {
            Property_List[Property_List_Index] = PROP_STRUCTURED_OBJECT_LIST;
            Property_List_Index++;
            Property_List_Length++;
        }
        if (bHasObjectList) {
            Property_List[Property_List_Index] = PROP_OBJECT_LIST;
            Property_List_Index++;
            Property_List_Length++;
        }
        /* Now insert the -1 list terminator, but don't count it. */
        Property_List[Property_List_Index] = -1;
        assert(Property_List_Length < MAX_PROPS);
        Property_List_Index = 0;        /* Will start at top of the list */
        nextState = GET_PROPERTY_REQUEST;
    }
    return nextState;
}

/* Initialize fields for a new Object */
void StartNextObject(
    BACNET_READ_ACCESS_DATA * rpm_object,
    BACNET_OBJECT_ID * pNewObject)
{

    BACNET_PROPERTY_REFERENCE *rpm_property;
#ifdef IOT
    get_prop_object();
#endif
    Error_Detected = false;
    Property_List_Index = Property_List_Length = 0;
    rpm_object->object_type = pNewObject->type;
    rpm_object->object_instance = pNewObject->instance;
    rpm_property = calloc(1, sizeof(BACNET_PROPERTY_REFERENCE));
    rpm_object->listOfProperties = rpm_property;
    rpm_object->next = NULL;
    assert(rpm_property);
    rpm_property->propertyIdentifier = PROP_ALL;
    rpm_property->propertyArrayIndex = BACNET_ARRAY_ALL;
}

/** Main function of the bacepics program.
 *
 * @see Device_Set_Object_Instance_Number, Keylist_Create, address_init,
 *      dlenv_init, address_bind_request, Send_WhoIs,
 *      tsm_timer_milliseconds, datalink_receive, npdu_handler,
 *      Send_Read_Property_Multiple_Request,
 *
 *
 * @param argc [in] Arg count.
 * @param argv [in] Takes one to four arguments, processed in CheckCommandLineArgs().
 * @return 0 on success.
 */

int epics(IOT_DEVICE_OBJECT_D2C dev)
{
    BACNET_ADDRESS src; /* address where message came from */
    uint16_t pdu_len = 0;
    unsigned timeout = 100;     /* milliseconds */
    unsigned max_apdu = 0;
    time_t elapsed_seconds = 0;
    time_t last_seconds = 0;
    time_t current_seconds = 0;
    time_t timeout_seconds = 0;
    bool found = false;
    BACNET_OBJECT_ID myObject;
    uint8_t buffer[MAX_PDU] = { 0 };
    BACNET_READ_ACCESS_DATA *rpm_object = NULL;
    KEY nextKey;
#ifdef IOT
    myiotdeviceobject = dev;
    if(dev != NULL)
        myiotpropertylist = dev->iot_prop_list;
    target_device_obj_ins = myiotdeviceobject->device_id;
#endif

    // CheckCommandLineArgs(argc, argv);   /* Won't return if there is an issue. */
    memset(&src, 0, sizeof(BACNET_ADDRESS));

    /* setup my info */
    Object_List = Keylist_Create();
#if defined(BACDL_BIP)
    /* For BACnet/IP, we might have set a different port for "me", so
     * (eg) we could talk to a BACnet/IP device on our same interface.
     * My_BIP_Port will be non-zero in this case.
     */
    if (My_BIP_Port > 0) {
        bip_set_port(htons(My_BIP_Port));
    }
#endif
    Init_Service_Handlers();
    /* configure the timeout values */
    current_seconds = time(NULL);
    timeout_seconds = (apdu_timeout() / 1000) * apdu_retries();

#if defined(BACDL_BIP)
    if (My_BIP_Port > 0) {
        bip_set_port(htons(0xBAC0));    /* Set back to std BACnet/IP port */
    }
#endif
    /* try to bind with the target device */
     printf("send bind request to = %d\n", target_device_obj_ins);
    found =
        address_bind_request(target_device_obj_ins, &max_apdu,
        &Target_Address);
        printf("found = %d\n", found);
    if (!found) {
        if (Provided_Targ_MAC) {
            if (Target_Address.net > 0) {
                /* We specified a DNET; call Who-Is to find the full
                 * routed path to this Device */
                Send_WhoIs_Remote(&Target_Address,
                    target_device_obj_ins,
                    target_device_obj_ins);

            } else {
                /* Update by adding the MAC address */
                if (max_apdu == 0)
                    max_apdu = MAX_APDU;        /* Whatever set for this datalink. */
                address_add_binding(target_device_obj_ins, max_apdu,
                    &Target_Address);
            }
        } else {
            Send_WhoIs(target_device_obj_ins,
                target_device_obj_ins);
        }
    }
    myObject.type = OBJECT_DEVICE;
    myObject.instance = target_device_obj_ins;
    myState = INITIAL_BINDING;
    do {
        /* increment timer - will exit if timed out */
        last_seconds = current_seconds;
        current_seconds = time(NULL);
        /* Has at least one second passed ? */
        if (current_seconds != last_seconds) {
            tsm_timer_milliseconds((uint16_t) ((current_seconds -
                        last_seconds) * 1000));
        }

        /* OK to proceed; see what we are up to now */
        switch (myState) {
            case INITIAL_BINDING:
                /* returns 0 bytes on timeout */
                 //printf("case INITIAL_BINDING\n");
                 pdu_len =
                     datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);
                // /* process; normally is some initial error */
                 if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                 }
                /* will wait until the device is bound, or timeout and quit */
                //printf("send bind request 2 to = %d\n", target_device_obj_ins);
                found =
                    address_bind_request(target_device_obj_ins,
                    &max_apdu, &Target_Address);
                    //printf("found 2 = %d\n", found);
                if (!found) {

                    /* increment timer - exit if timed out */
                    elapsed_seconds += (current_seconds - last_seconds);
                    if (elapsed_seconds > timeout_seconds) {
                        fprintf(stderr,
                            "\rError: Unable to bind to %u"
                            " after waiting %ld seconds.\n",
                            target_device_obj_ins,
                            (long int) elapsed_seconds);
                        return 1;//break;
                    }
                    /* else, loop back and try again */
                    continue;
                } else {
                    rpm_object = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
                    assert(rpm_object);
                    myState = GET_HEADING_INFO;
                }
                break;

            case GET_HEADING_INFO:
                /* FIXME: get heading properties with ReadProperty */
                // printf("case GET_HEADING_INFO\n");
                last_seconds = current_seconds;
                StartNextObject(rpm_object, &myObject);
                BuildPropRequest(rpm_object);
                Request_Invoke_ID =
                    Send_Read_Property_Multiple_Request(buffer, MAX_PDU,
                    target_device_obj_ins, rpm_object);
                if (Request_Invoke_ID > 0) {
                    elapsed_seconds = 0;
                } else {
                    /* We failed.  Will hurt the header info we can show. */
                    fprintf(stderr, "\r-- Failed to get Heading info \n");
                }
                myState = GET_HEADING_RESPONSE;
                break;

            case PRINT_HEADING:
                /* Print out the header information */
                 //printf("case PRINT_HEADING\n");
                // PrintHeading();
                myState = GET_ALL_REQUEST;
                /* Fall through now */

            case GET_ALL_REQUEST:
            case GET_LIST_OF_ALL_REQUEST:
            //  if(myState == GET_ALL_REQUEST) printf("case GET_ALL_REQUEST\n");
            //  if(myState == GET_LIST_OF_ALL_REQUEST) printf("case GET_LIST_OF_ALL_REQUEST\n");
                /* "list" differs in ArrayIndex only */
                /* Update times; aids single-step debugging */
                last_seconds = current_seconds;
                StartNextObject(rpm_object, &myObject);

                Request_Invoke_ID =
                    Send_Read_Property_Multiple_Request(buffer, MAX_PDU,
                    target_device_obj_ins, rpm_object);
                if (Request_Invoke_ID > 0) {
                    elapsed_seconds = 0;
                    if (myState == GET_LIST_OF_ALL_REQUEST)
                        myState = GET_LIST_OF_ALL_RESPONSE;
                    else
                        myState = GET_ALL_RESPONSE;
                }
                break;

            case GET_HEADING_RESPONSE:
            case GET_ALL_RESPONSE:
            case GET_LIST_OF_ALL_RESPONSE:
                /* returns 0 bytes on timeout */
                //  if(myState == GET_HEADING_RESPONSE) printf("case GET_HEADING_RESPONSE\n");
                //  if(myState == GET_ALL_RESPONSE) printf("case GET_ALL_RESPONSE\n");
                //  if(myState == GET_LIST_OF_ALL_RESPONSE) printf("case GET_LIST_OF_ALL_RESPONSE\n");
                pdu_len =
                    datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

                /* process */
                if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                }

                if ((Read_Property_Multiple_Data.new_data) &&
                    (Request_Invoke_ID ==
                        Read_Property_Multiple_Data.service_data.invoke_id)) {
                    Read_Property_Multiple_Data.new_data = false;
                   // printf("Call ProcessRPMData\n");
                    myState =
                        ProcessRPMData(Read_Property_Multiple_Data.rpm_data,
                        myState);
                    if (tsm_invoke_id_free(Request_Invoke_ID)) {
                        Request_Invoke_ID = 0;
                    } else {
                        assert(false);  /* How can this be? */
                        Request_Invoke_ID = 0;
                    }
                    elapsed_seconds = 0;
                } else if (tsm_invoke_id_free(Request_Invoke_ID)) {
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else if (Error_Detected) {
                        if (Last_Error_Code ==
                            ERROR_CODE_REJECT_UNRECOGNIZED_SERVICE) {
                            /* The normal case for Device Object */
                            /* Was it because the Device can't do RPM? */
                            Has_RPM = false;
                            myState = GET_PROPERTY_REQUEST;
                        } else if (Last_Error_Code ==
                            ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED) {
                            myState = GET_PROPERTY_REQUEST;
                            StartNextObject(rpm_object, &myObject);
                        } else if (myState == GET_ALL_RESPONSE)
                            /* Try again, just to get a list of properties. */
                            myState = GET_LIST_OF_ALL_REQUEST;
                        else {
                            /* Else drop back to RP. */
                            myState = GET_PROPERTY_REQUEST;
                            StartNextObject(rpm_object, &myObject);
                        }
                    } else if (Has_RPM)
                        myState = GET_ALL_REQUEST;      /* Let's try again */
                    else
                        myState = GET_PROPERTY_REQUEST;
                } else if (tsm_invoke_id_failed(Request_Invoke_ID)) {
                    fprintf(stderr, "\rError: TSM Timeout!\n");
                    tsm_free_invoke_id(Request_Invoke_ID);
                    Request_Invoke_ID = 0;
                    elapsed_seconds = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else
                        myState = GET_ALL_REQUEST;      /* Let's try again */
                } else if (Error_Detected) {
                    /* Don't think we'll ever actually reach this point. */
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else
                        myState = NEXT_OBJECT;  /* Give up and move on to the next. */
                    Error_Count++;
                }
                break;

                /* Process the next single property in our list,
                 * if we couldn't GET_ALL at once above. */
            case GET_PROPERTY_REQUEST:
            //  printf("case GET_PROPERTY_REQUEST\n");
                Error_Detected = false;
                /* Update times; aids single-step debugging */
                last_seconds = current_seconds;
                elapsed_seconds = 0;
                Request_Invoke_ID =
                    Read_Properties(target_device_obj_ins, &myObject);
                if (Request_Invoke_ID == 0) {
                    /* Reached the end of the list. */
                    myState = NEXT_OBJECT;      /* Move on to the next. */
                } else
                    myState = GET_PROPERTY_RESPONSE;
                break;

            case GET_PROPERTY_RESPONSE:
            //  printf("case GET_PROPERTY_RESPONSE\n");
                /* returns 0 bytes on timeout */
                pdu_len =
                    datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

                /* process */
                if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                }

                if ((Read_Property_Multiple_Data.new_data) &&
                    (Request_Invoke_ID ==
                        Read_Property_Multiple_Data.service_data.invoke_id)) {
                    Read_Property_Multiple_Data.new_data = false;
                    PrintReadPropertyData(Read_Property_Multiple_Data.
                        rpm_data->object_type,
                        Read_Property_Multiple_Data.rpm_data->object_instance,
                        Read_Property_Multiple_Data.rpm_data->
                        listOfProperties);
                    if (tsm_invoke_id_free(Request_Invoke_ID)) {
                        Request_Invoke_ID = 0;
                    } else {
                        assert(false);  /* How can this be? */
                        Request_Invoke_ID = 0;
                    }
                    elapsed_seconds = 0;
                    /* Advance the property (or Array List) index */
                    if (Using_Walked_List) {
                        Walked_List_Index++;
                        if (Walked_List_Index > Walked_List_Length) {
                            /* go on to next property */
                            Property_List_Index++;
                            Using_Walked_List = false;
                        }
                    } else {
                        Property_List_Index++;
                    }
                    myState = GET_PROPERTY_REQUEST;     /* Go fetch next Property */
                } else if (tsm_invoke_id_free(Request_Invoke_ID)) {
                    Request_Invoke_ID = 0;
                    elapsed_seconds = 0;
                    myState = GET_PROPERTY_REQUEST;
                    if (Error_Detected) {
                        if ((Last_Error_Class != ERROR_CLASS_PROPERTY) &&
                            (Last_Error_Code != ERROR_CODE_UNKNOWN_PROPERTY)) {
                            if (IsLongArray) {
                                /* Change to using a Walked List and retry this property */
                                Using_Walked_List = true;
                                Walked_List_Index = Walked_List_Length = 0;
                            } else {
                                /* OK, skip this one and try the next property. */
                                fprintf(stdout, "    -- Failed to get ");
                                Print_Property_Identifier(Property_List
                                    [Property_List_Index]);
                                fprintf(stdout, " \n");
                                Error_Count++;
                                Property_List_Index++;
                                if (Property_List_Index >=
                                    Property_List_Length) {
                                    /* Give up and move on to the next. */
                                    myState = NEXT_OBJECT;
                                }
                            }
                        } else {
                            fprintf(stdout, "    -- unknown property\n");
                            Error_Count++;
                            Property_List_Index++;
                            if (Property_List_Index >= Property_List_Length) {
                                /* Give up and move on to the next. */
                                myState = NEXT_OBJECT;
                            }
                        }
                    }
                } else if (tsm_invoke_id_failed(Request_Invoke_ID)) {
                    fprintf(stderr, "\rError: TSM Timeout!\n");
                    tsm_free_invoke_id(Request_Invoke_ID);
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    myState = 3;        /* Let's try again, same Property */
                } else if (Error_Detected) {

                    /* Don't think we'll ever actually reach this point. */
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    myState = NEXT_OBJECT;      /* Give up and move on to the next. */
                    Error_Count++;
                }
                break;

            case NEXT_OBJECT:
            //  printf("case NEXT_OBJECT\n");
            //  printf("OBJECT Type = %d\n", myObject.type);
            //  printf("Object_List_Index = %d\n", Object_List_Index);
                if (myObject.type == OBJECT_DEVICE) {
                    printf("  -- Found %d Objects \n",
                        Keylist_Count(Object_List));
                    Object_List_Index = -1;     /* start over (will be incr to 0) */
                }
                /* Advance to the next object, as long as it's not the Device object */
                do {
                    Object_List_Index++;
                    if (Object_List_Index < Keylist_Count(Object_List)) {
                        nextKey = Keylist_Key(Object_List, Object_List_Index);
                        myObject.type = KEY_DECODE_TYPE(nextKey);
                        myObject.instance = KEY_DECODE_ID(nextKey);
                        /* Don't re-list the Device Object among its objects */
                        if (myObject.type == OBJECT_DEVICE)
                            continue;
                        /* Closing brace for the previous Object */
                        printf("  }, \n");
                        /* Opening brace for the new Object */
                        printf("  { \n");
                        /* Test code:
                           if ( myObject.type == OBJECT_STRUCTURED_VIEW )
                           printf( "    -- Structured View %d \n", myObject.instance );
                         */
                    } else {
                        /* Closing brace for the last Object */
                        printf("  } \n");
                        /* done with all Objects, signal end of this while loop */
                        myObject.type = MAX_BACNET_OBJECT_TYPE;
                    }
                    if (Has_RPM)
                        myState = GET_ALL_REQUEST;
                    else {
                        myState = GET_PROPERTY_REQUEST;
                        StartNextObject(rpm_object, &myObject);
                    }

                } while (myObject.type == OBJECT_DEVICE);
                /* Else, don't re-do the Device Object; move to the next object. */
                break;

            default:
                assert(false);  /* program error; fix this */
                break;
        }

        /* Check for timeouts */
        if (!found || (Request_Invoke_ID > 0)) {
            /* increment timer - exit if timed out */
            elapsed_seconds += (current_seconds - last_seconds);
            if (elapsed_seconds > timeout_seconds) {
                fprintf(stderr, "\rError: APDU Timeout! (%lds)\n",
                    (long int) elapsed_seconds);
                return 1;//break;break;
            }
        }

    } while (myObject.type < MAX_BACNET_OBJECT_TYPE);

    if (Error_Count > 0)
        fprintf(stdout, "\r-- Found %d Errors \n", Error_Count);

    /* Closing brace for all Objects, if we got any, and closing footer  */
    if (myState != INITIAL_BINDING) {
        printf("} \n");
        printf
            ("End of BACnet Protocol Implementation Conformance Statement\n");
        //printf("\n");
        printf("\n");
    }

    return 0;
}

/*@} */

/* End group BACEPICS */

