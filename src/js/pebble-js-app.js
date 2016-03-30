//JSLint static code analysis options
/*jslint browser:true, unparam:true, sloppy:true, plusplus:true, indent:4, white:true */
/*global Pebble, console*/

var VERSION = 10,   //i.e. v1.0; for sending to config page
    //Defaults:
    DEF_VIBES = 0X00A14,
    watchConfig = {
        KEY_VIBES: DEF_VIBES
    },
    //masks for vibes:
    MASKV_BTDC = 0x20000,
    MASKV_HOURLY = 0x10000,
    MASKV_FROM = 0xFF00,
    MASKV_TO = 0x00FF;

//Load saved config from localStorage
function loadConfig()
{
    var config = localStorage.getItem(0);
    if (config)
    {
        if (typeof config === 'string')
        {
            config = JSON.parse(config);
        }
        if (config.KEY_VIBES !== undefined)
        {
            watchConfig = config;
        }
    }
}

//Save config to localStorage
function saveConfig()
{
    localStorage.setItem(0, JSON.stringify(watchConfig));
}

function sendOptions(options)
{
    Pebble.sendAppMessage({
            KEY_VIBES: options.KEY_VIBES
        },
        function(e) {
            console.log('Successfully delivered message');
        },
        function(e) {
            console.log('Unable to deliver message');
        }
    );
}

Pebble.addEventListener('webviewclosed',
    function(e) {
        //console.log('Configuration window returned: ' + e.response);
        if (!e.response)
        {
            return;
        }
        var options = JSON.parse(e.response),
            noOptions = true,
            value;
        if (options.vibes !== undefined)
        {
            value = parseInt(options.vibes, 10);
            watchConfig.KEY_VIBES = value;
            noOptions = false;
        }
        if (noOptions)
        {
            return;
        }

        saveConfig();
        sendOptions(watchConfig);
    }
);


Pebble.addEventListener('showConfiguration',
    function(e) {
        try {
            var url = 'http://yunharla.altervista.org/pebble/config-bar.html?ver=' + VERSION + '&vibes=0x';
            //Send/show current config in config page:
            url += watchConfig.KEY_VIBES.toString(16);
            //console.log(url);

            // Show config page
            Pebble.openURL(url);
        }
        catch (ex)
        {
            console.log('ERR: showConfiguration exception');
        }
    }
);

Pebble.addEventListener('ready',
    function(e) {
        console.log('ready');
        loadConfig();
        sendOptions(watchConfig);
    });
