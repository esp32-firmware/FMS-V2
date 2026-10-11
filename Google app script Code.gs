// =================================================================================
// SENSEI GOOGLE APPS SCRIPT - FEE MANAGEMENT SYSTEM
// =================================================================================

var SHEET_NAME = "Fees Record"; 
var COL_NAME = 2; // Column B (Name)
var COL_UID  = 3; // Column C (UID)
const SHEET_ID = "1OFUIQTQn0D2sdE5KcZ6GKP6bG7QtZ4Llq1nBQ9_35cY";

// --- COMBINED doGet FUNCTION ---
// Handles both ESP32 hardware (API parameters) and the Online Web Portal (HTML)
function doGet(e) {
  // 1. Check if the request comes from the ESP32 (has parameters)
  if (e && e.parameter && Object.keys(e.parameter).length > 0) {
    if (e.parameter.action == "get_names") {
      return handleGetNames();
    }
    
    var uid = e.parameter.uid;
    var month = e.parameter.month;
    
    if (uid && month) {
      return handleFeeRecord(uid, month);
    }
    
    return ContentService.createTextOutput("ERROR: Invalid Request");
  }
  
  // 2. If no parameters are passed, serve the Web App UI for your portal
  return HtmlService.createTemplateFromFile('Index')
    .evaluate()
    .setTitle('TSKF Fees Record')
    .addMetaTag('viewport', 'width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0')
    .setXFrameOptionsMode(HtmlService.XFrameOptionsMode.ALLOWALL);
}

// ==========================================
// ESP32 API SEGMENT (GET)
// ==========================================

function handleGetNames() {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  var data = sheet.getDataRange().getValues();
  var csvOutput = "";
  
  // Start at i = 1 to skip the header row
  for (var i = 1; i < data.length; i++) {
    var currentUid = data[i][COL_UID - 1];   
    var currentName = data[i][COL_NAME - 1];
    
    // THE STRICT GATEKEEPER:
    // Only process this row if BOTH the UID and the Name exist and are not blank.
    if (currentUid && currentUid.toString().trim() !== "" && 
        currentName && currentName.toString().trim() !== "") {
      
      var cleanUid = currentUid.toString().trim();
      var cleanName = currentName.toString().trim();
      
      csvOutput += cleanUid + "," + cleanName + "\n";
    }
  }
  return ContentService.createTextOutput(csvOutput);
}

function handleFeeRecord(targetUid, targetMonth) {
  // --- TRAFFIC CONTROL LOCK ---
  // Prevents data corruption if multiple network retries or machines hit the sheet at the exact same millisecond.
  var lock = LockService.getScriptLock();
  lock.waitLock(10000); // Wait up to 10 seconds if another upload is currently processing
  
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
    var data = sheet.getDataRange().getValues();
    var headers = data[0]; 
    
    var foundRow = -1;
    var studentName = "Unknown";
    
    // STEP A: Search for existing UID
    for (var i = 1; i < data.length; i++) {
      var currentUid = data[i][COL_UID - 1];
      if (currentUid != "" && currentUid.toString().toUpperCase() == targetUid.toUpperCase()) {
        foundRow = i + 1; 
        studentName = data[i][COL_NAME - 1];
        break;
      }
    }
    
    // STEP B: NEW UID (Strictly paste in UID column only)
    if (foundRow == -1) {
      var newRow = sheet.getLastRow() + 1;
      sheet.getRange(newRow, COL_UID).setValue(targetUid); // ONLY pastes the UID in Column C
      return ContentService.createTextOutput("REGISTERED_NEW");
    }
    
    // STEP C: EXISTING UID (Double-Entry Blocker & Mark "Paid")
    var monthCol = -1;
    for (var j = 0; j < headers.length; j++) {
      if (headers[j].toString().trim().toLowerCase() == targetMonth.toLowerCase()) {
        monthCol = j + 1; 
        break;
      }
    }
    
    if (monthCol != -1) {
      // --- NEW DOUBLE ENTRY BLOCKER ---
      // Check what is currently in the cell before overwriting it
      var currentStatus = sheet.getRange(foundRow, monthCol).getValue();
      
      if (currentStatus.toString().trim().toLowerCase() == "paid") {
        // If already paid, reject the overwrite and notify the machine
        return ContentService.createTextOutput("ALREADY_PAID:" + studentName);
      } else {
        // If empty or unpaid, mark as paid
        sheet.getRange(foundRow, monthCol).setValue("Paid");
        return ContentService.createTextOutput("SUCCESS:" + studentName);
      }
    }
    
    return ContentService.createTextOutput("ERROR: MONTH NOT FOUND");
    
  } catch (e) {
    return ContentService.createTextOutput("ERROR: System Fault - " + e.message);
  } finally {
    // --- RELEASE THE LOCK FOR THE NEXT UPLOAD ---
    lock.releaseLock();
  }
}

// ==========================================
// ESP32 API SEGMENT (POST) - BATCH UPLOAD
// ==========================================

function doPost(e) {
  if (e && e.postData && e.postData.contents) {
    return handleBatchUpload(e.postData.contents);
  }
  return ContentService.createTextOutput("ERROR: Invalid POST Data");
}

function handleBatchUpload(payload) {
  var lock = LockService.getScriptLock();
  lock.waitLock(15000); // Traffic control lock for 15 seconds

  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
    var data = sheet.getDataRange().getValues();
    var headers = data[0];

    var lines = payload.split("\n");
    var processedCount = 0;
    var updatesMade = false;

    // Map month headers to column indexes for lightning-fast lookup
    var monthCols = {};
    for (var j = 0; j < headers.length; j++) {
      monthCols[headers[j].toString().trim().toLowerCase()] = j;
    }

    // Process the entire batch in memory (prevents Google timeouts)
    for (var k = 0; k < lines.length; k++) {
      var parts = lines[k].trim().split(",");
      if (parts.length < 2) continue; // Skip empty/malformed lines

      var targetUid = parts[0].trim().toUpperCase();
      var targetMonth = parts[1].trim().toLowerCase();

      var foundRowIdx = -1;
      // Search for UID
      for (var i = 1; i < data.length; i++) {
        var currentUid = data[i][COL_UID - 1];
        if (currentUid && currentUid.toString().trim().toUpperCase() == targetUid) {
          foundRowIdx = i;
          break;
        }
      }

      // If UID is completely new, create a blank row for it in memory
      if (foundRowIdx == -1) {
        var newRow = new Array(headers.length).fill("");
        newRow[COL_UID - 1] = targetUid;
        data.push(newRow);
        foundRowIdx = data.length - 1;
        updatesMade = true;
      }

      // Mark the target month as "Paid"
      if (monthCols.hasOwnProperty(targetMonth)) {
        var mCol = monthCols[targetMonth];
        if (data[foundRowIdx][mCol].toString().trim().toLowerCase() !== "paid") {
          data[foundRowIdx][mCol] = "Paid";
          updatesMade = true;
        }
        processedCount++; // Successfully counted this line
      }
    }

    // Write all changes back to the sheet in a single fast operation
    if (updatesMade) {
      sheet.getRange(1, 1, data.length, data[0].length).setValues(data);
    }

    // Send the exact syntax the ESP32 needs to delete records from /queue.txt
    return ContentService.createTextOutput("SUCCESS:" + processedCount);

  } catch (e) {
    return ContentService.createTextOutput("ERROR: System Fault - " + e.message);
  } finally {
    lock.releaseLock();
  }
}

// ==========================================
// WEB APP SEGMENT - FEES RECORD
// ==========================================

function getSheet() {
  return SpreadsheetApp.openById(SHEET_ID).getSheetByName("Fees Record");
}

// Fetches all unique branches and student names mapped to their branches
function getInitialData() {
  var sheet = getSheet();
  var data = sheet.getDataRange().getValues();
  
  var branches = {}; 
  var students = [];

  // Data starts at row 2 (index 1) based on the image structure
  for (var i = 1; i < data.length; i++) {
    var name = data[i][1] ? data[i][1].toString().trim() : ""; // Column B
    var branch = data[i][3] ? data[i][3].toString().trim() : ""; // Column D
    
    if (name && branch) {
      branches[branch] = true; // Collect unique branches
      students.push({ name: name, branch: branch });
    }
  }
  
  return {
    branches: Object.keys(branches).sort(),
    students: students
  };
}

function searchStudent(name, branch) {
  var sheet = getSheet();
  var data = sheet.getDataRange().getValues();
  var results = [];
  var monthsHeaders = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];
  
  for (var i = 1; i < data.length; i++) {
    var rowName = data[i][1] ? data[i][1].toString().trim() : "";
    var rowBranch = data[i][3] ? data[i][3].toString().trim() : "";
    
    // Ensure both Name and Branch match
    if (rowName.toLowerCase() === name.toLowerCase() && rowBranch === branch) {
      var monthsStatus = {};
      for (var m = 0; m < 12; m++) {
        // January starts at Column I (Index 8) based on the provided image
        var statusVal = data[i][8 + m] ? data[i][8 + m].toString().trim() : "";
        monthsStatus[monthsHeaders[m]] = {
          status: statusVal,
          colIndex: 9 + m // Actual Column Number for Google Sheets
        };
      }
      results.push({
        row: i + 1, 
        name: rowName,
        contact: data[i][5] || "N/A", // Contact is in Column F (Index 5)
        months: monthsStatus
      });
      break; 
    }
  }
  return results;
}

// Updated logging context to reflect branch instead of admin
function updatePaymentState(row, col, status, branch, studentName, month, method) {
  var ss = SpreadsheetApp.openById(SHEET_ID);
  var sheet = ss.getSheetByName("Fees Record");
  
  sheet.getRange(row, col).setValue(status === "Not Paid" ? "" : status);

  // Log the transaction if marked as Paid
  if (status === "Paid") {
    var logSheet = ss.getSheetByName("Logs");
    
    if (!logSheet) {
      logSheet = ss.insertSheet("Logs");
      logSheet.appendRow(["Timestamp", "Branch", "Student Name", "Month", "Payment Method", "Status"]);
      logSheet.getRange("A1:F1").setFontWeight("bold");
      logSheet.setColumnWidth(1, 160);
      logSheet.setColumnWidth(3, 200);
    }
    
    logSheet.appendRow([new Date(), branch, studentName, month, method, status]);
  }

  return status;
}
