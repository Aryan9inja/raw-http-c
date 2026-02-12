const statusText = document.getElementById("status-text");
const pingButton = document.getElementById("ping-btn");
const logList = document.getElementById("log-list");

function addLog(message) {
  const item = document.createElement("li");
  item.textContent = message;
  logList.prepend(item);
}

function updateStatus(message) {
  statusText.textContent = message;
}

function pingServer() {
  const timestamp = new Date().toISOString();
  updateStatus("Ping sent at " + timestamp);
  addLog("Ping clicked at " + timestamp);
}

pingButton.addEventListener("click", pingServer);

updateStatus("Ready. Click Ping to record a test event.");
