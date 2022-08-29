
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

 }

