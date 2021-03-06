if (this.importScripts) {
    importScripts('../../../fast/js/resources/js-test-pre.js');
    importScripts('shared.js');
}

removeVendorPrefixes();
dbname = decodeURIComponent(self.location.search.substring(1));
evalAndLog("request = indexedDB.open(\"" + dbname + "\", 2)");
request.onupgradeneeded = unexpectedUpgradeNeededCallback;
request.onblocked = function(e) {
    testPassed("worker received blocked event.");
    finishJSTest();
};
