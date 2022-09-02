
var uRLPrefix = "http://" + location.hostname + "/";

var tzLook = {};

function removeOptions(selectElement) {

    var i, L = selectElement.options.length - 1;
    for(i = L; i >= 0; i--) {
       selectElement.remove(i);
    }
 }
 function setSelectToValues(selRef, lst) {
     removeOptions(selRef);
     for (const val in lst) {
     var el = document.createElement("option");
     el.textContent = val;
     if ( typeof(lst[val]) == "number" ) {
         el.value = lst[val];
     } else {
         el.value = val;
     }
     selRef.appendChild(el);
     }
     selRef.selectedIndex = "0"
 }
 function onAreaChange() {
    setSelectToValues(document.getElementById('city'), tzLook[area.value])
 }
 
 function setupConfig() {

    fetch(uRLPrefix + 'timezone.json').then(function (response) {
        return response.json();
    }).then(function (msg) {

        for (var curKey in msg ) {
            spl = curKey.split('/');
            region = spl[0]
            city = spl[1]

            if ( region in tzLook ) {
                ;
            } else {
                tzLook[region] = {};
            }
            tzLook[region][city] = msg[curKey];
        }


        setSelectToValues(area, tzLook);
        browserTz = Intl.DateTimeFormat().resolvedOptions().timeZone.split("/");
        area.value = browserTz[0];
        onAreaChange();
        document.getElementById('city').value = browserTz[1];
      
    });
    var today = new Date();
    document.getElementById('inputhour').value = today.getHours() % 12;
    document.getElementById('inputminute').value = today.getMinutes();
    document.getElementById('inputsecond').value = today.getSeconds();

 }

function onSave() {

    console.log("On save");
    var msg = {};

    msg["clockhands"] = {};
    msg["clockhands"]["hour"] = document.getElementById('inputhour').value;
    msg["clockhands"]["minute"] = document.getElementById('inputminute').value;
    msg["clockhands"]["second"] = document.getElementById('inputsecond').value;


    msg["timezone"] = {};
    msg["timezone"]["region"] = document.getElementById('area').value;
    msg["timezone"]["city"] = document.getElementById('city').value;
    msg["timezone"]["string"] = tzLook[document.getElementById('area').value][document.getElementById('city').value];


    fetch(uRLPrefix + 'api/config', {
        method: 'put',
        body: JSON.stringify(msg)
    }).then(function (response) {
        if (response.ok) {
            window.location.replace("/");
        } else {
            document.getElementById('statusmessage').value = "Error saving, response logged to console";
            console.log(response)
        }
    });


}