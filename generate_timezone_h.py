from concurrent.futures import process
import json

POSTION_ZERO_TIMEZONE = "Etc/Universal" 

OUTPUT_HEADER_FILENAME = "src/Generated_Timezones.hpp"
INPUT_FILE_NAME = ".pio/libdeps/d1_mini/posix_tz_db/zones.json"

HEADER_TEMPLATE = """

/* This is a generated header file, any edits will be overwritten.

To re-generate, run:
                    generate_timezone_h.py 
                    
This uses the timezone generated from  https://github.com/nayarsystems/posix_tz_db . PlatformIO will auto-fetch the library when it is updated but this file
needs to be run manually.

*/



const char *GENERATED_TZ_LOOKUP[{}] = {};


const char *GENERATED_TZ_JSON = "{}";

const byte NEW_ZEALAND_TIMEZONE_POS = {};

"""

with open(INPUT_FILE_NAME, "r") as f:
    zones = json.load(f)


    # Force "Etc/Universal" timezone to always be position zero as its the default
    sortedTimezones = list(zones.keys())
    sortedTimezones.sort()
    sortedTimezones.remove(POSTION_ZERO_TIMEZONE)
    sortedTimezones.insert(0, POSTION_ZERO_TIMEZONE)


    # Build up dict with area to city mapping to timezone lookup array position
    # Place timezones in mapping as position
    # Duplicates are removed as they occur, this saves memory space and allows position be kept within a byte (<254)
    processed = dict()
    mapping = []
    lookupRef = dict()

    for tzKey in sortedTimezones:
        area, city = tzKey.split("/", maxsplit=1)

        areaDict = processed.get(area, None)
        if areaDict == None:
            areaDict = dict()
            processed[area] = areaDict

        if zones[tzKey] not in lookupRef:
            lookupRef[zones[tzKey]] = len(mapping)
            mapping.append(zones[tzKey])

        areaDict[city] = lookupRef[zones[tzKey]]


    lookup_size = len(mapping)

    mapping = map(lambda x: "\"{}\"".format(x), mapping)
    lookup_data = ",\n\t\t".join(mapping)
    lookup_data = "{" + lookup_data + "}"

    jsonForC = json.dumps(processed)
    jsonForC = jsonForC.replace("\"", "\\\"")

    nzTZPosition = processed["Pacific"]["Auckland"]


    with open(OUTPUT_HEADER_FILENAME, "w") as out:
        out.write(HEADER_TEMPLATE.format(lookup_size, lookup_data, jsonForC, nzTZPosition))



