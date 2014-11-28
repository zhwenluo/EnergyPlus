// ObjexxFCL Headers

// EnergyPlus Headers
#include "SQLiteProcedures.hh"
#include "DataGlobals.hh"
#include "DataStringGlobals.hh"
#include "DataEnvironment.hh"
#include "DataHeatBalance.hh"
#include "DataPrecisionGlobals.hh"
#include "DataRoomAirModel.hh"
#include "InputProcessor.hh"
#include "UtilityRoutines.hh"
#include "General.hh"
#include "ScheduleManager.hh"
#include "DataSystemVariables.hh"

#include <sstream>
#include <stdexcept>

namespace EnergyPlus {

const int SQLite::LocalReportEach     = -1;   // Write out each time UpdatedataandLocalReport is called
const int SQLite::LocalReportTimeStep =  0;   // Write out at 'EndTimeStepFlag'
const int SQLite::LocalReportHourly   =  1;   // Write out at 'EndHourFlag'
const int SQLite::LocalReportDaily    =  2;   // Write out at 'EndDayFlag'
const int SQLite::LocalReportMonthly  =  3;   // Write out at end of month (must be determined)
const int SQLite::LocalReportSim      =  4;   // Write out once per environment 'EndEnvrnFlag'
const int SQLite::ReportNameId        =  1;
const int SQLite::ReportForStringId   =  2;
const int SQLite::TableNameId         =  3;
const int SQLite::RowNameId           =  4;
const int SQLite::ColumnNameId        =  5;
const int SQLite::UnitsId             =  6;

std::unique_ptr<SQLite> sqlite;

SQLite::SQLite()
	:
	m_writeOutputToSQLite(false),
	m_writeTabularDataToSQLite(false),
	m_sqlDBTimeIndex(0),
	m_db(nullptr),
	m_dbName("eplusout.sql"),
	m_reportDataInsertStmt(nullptr),
	m_reportExtendedDataInsertStmt(nullptr),
	m_reportDictionaryInsertStmt(nullptr),
	m_timeIndexInsertStmt(nullptr),
	m_zoneInfoInsertStmt(nullptr),
	m_nominalLightingInsertStmt(nullptr),
	m_nominalElectricEquipmentInsertStmt(nullptr),
	m_nominalGasEquipmentInsertStmt(nullptr),
	m_nominalSteamEquipmentInsertStmt(nullptr),
	m_nominalHotWaterEquipmentInsertStmt(nullptr),
	m_nominalOtherEquipmentInsertStmt(nullptr),
	m_nominalBaseboardHeatInsertStmt(nullptr),
	m_surfaceInsertStmt(nullptr),
	m_constructionInsertStmt(nullptr),
	m_constructionLayerInsertStmt(nullptr),
	m_materialInsertStmt(nullptr),
	m_zoneListInsertStmt(nullptr),
	m_zoneGroupInsertStmt(nullptr),
	m_infiltrationInsertStmt(nullptr),
	m_ventilationInsertStmt(nullptr),
	m_nominalPeopleInsertStmt(nullptr),
	m_zoneSizingInsertStmt(nullptr),
	m_systemSizingInsertStmt(nullptr),
	m_componentSizingInsertStmt(nullptr),
	m_roomAirModelInsertStmt(nullptr),
	m_groundTemperatureInsertStmt(nullptr),
	m_weatherFileInsertStmt(nullptr),
	m_scheduleInsertStmt(nullptr),
	m_daylightMapTitleInsertStmt(nullptr),
	m_daylightMapHorlyTitleInsertStmt(nullptr),
	m_daylightMapHorlyDataInsertStmt(nullptr),
	m_environmentPeriodInsertStmt(nullptr),
	m_simulationsInsertStmt(nullptr),
	m_tabularDataInsertStmt(nullptr),
	m_stringsInsertStmt(nullptr),
	m_stringsLookUpStmt(nullptr),
	m_errorInsertStmt(nullptr),
	m_errorUpdateStmt(nullptr),
	m_simulationUpdateStmt(nullptr)
{
	int numberOfSQLiteObjects = InputProcessor::GetNumObjectsFound("Output:SQLite");

	if((numberOfSQLiteObjects == 1) && (! DataSystemVariables::DDOnly)) {
		FArray1D_string alphas(5);
		int numAlphas;
		FArray1D< Real64 > numbers(2);
		int numNumbers;
		int status;

		InputProcessor::GetObjectItem("Output:SQLite",1,alphas,numAlphas,numbers,numNumbers,status);
		if( numAlphas > 0 ) {
			std::string option = alphas(1);
			if( InputProcessor::SameString(option,"SimpleAndTabular") ) {
				m_writeTabularDataToSQLite = true;
				m_writeOutputToSQLite = true;
			} else if( InputProcessor::SameString(option,"Simple") ) {
				m_writeOutputToSQLite = true;
			}
		}
	}

	if( m_writeOutputToSQLite ) {
		int rc = -1;
		bool ok = true;
		m_errorStream.open("sqlite.err", std::ofstream::out | std::ofstream::trunc);

		// Test if we can write to the sqlite error file
		//  Does there need to be a seperate sqlite.err file at all?  Consider using eplusout.err
		if( m_errorStream.is_open() ) {
			m_errorStream << "SQLite3 message, sqlite.err open for processing!" << std::endl;
		} else {
			ok = false;
		}

		// Test if we can create a new file named m_dbName
		if( ok ) {
			std::ofstream test(m_dbName, std::ofstream::out | std::ofstream::trunc);
			if( test.is_open() ) {
				test.close();
			} else {
				ok = false;
			}
		}

		// Test if we can write to the database
		// If we can't then there are probably locks on the database
		if( ok ) {
			sqlite3_open_v2(m_dbName.c_str(), &m_db, SQLITE_OPEN_READWRITE, nullptr);
			char * zErrMsg = nullptr;
			rc = sqlite3_exec(m_db, "CREATE TABLE Test(x INTEGER PRIMARY KEY)", nullptr, 0, &zErrMsg);
			sqlite3_close(m_db);
			if( rc ) {
				m_errorStream << "SQLite3 message, can't get exclusive lock on existing database: " << sqlite3_errmsg(m_db) << std::endl;
				ok = false;
			} else {
				// Remove test db
				rc = remove( m_dbName.c_str() );
				if( rc ) {
					m_errorStream << "SQLite3 message, can't remove old database: " << sqlite3_errmsg(m_db) << std::endl;
					ok = false;
				}
			}
			sqlite3_free(zErrMsg);
		}

		if( ok ) {
			// Now open the output db for the duration of the simulation
			rc = sqlite3_open_v2(m_dbName.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
			if( rc ) {
				m_errorStream << "SQLite3 message, can't open new database: " << sqlite3_errmsg(m_db) << std::endl;
				sqlite3_close(m_db);
				ok = false;
			}
		}

		if( ok ) {
			sqliteExecuteCommand("PRAGMA locking_mode = EXCLUSIVE;");
			sqliteExecuteCommand("PRAGMA journal_mode = OFF;");
			sqliteExecuteCommand("PRAGMA synchronous = OFF;");

			initializeReportDataDictionaryTable();
			initializeReportDataTables();
			initializeTimeIndicesTable();
			initializeZoneInfoTable();
			initializeNominalPeopleTable();
			initializeNominalLightingTable();
			initializeNominalElectricEquipmentTable();
			initializeNominalGasEquipmentTable();
			initializeNominalSteamEquipmentTable();
			initializeNominalHotWaterEquipmentTable();
			initializeNominalOtherEquipmentTable();
			initializeNominalBaseboardHeatTable();
			initializeSurfacesTable();
			initializeConstructionsTables();
			initializeMaterialsTable();
			initializeZoneListTable();
			initializeZoneGroupTable();
			initializeNominalInfiltrationTable();
			initializeNominalVentilationTable();
			initializeZoneSizingTable();
			initializeSystemSizingTable();
			initializeComponentSizingTable();
			initializeRoomAirModelTable();
			initializeSchedulesTable();
			initializeDaylightMapTables();
			initializeViews();
			initializeSimulationsTable();
			initializeEnvironmentPeriodsTable();
			initializeErrorsTable();

			if(m_writeTabularDataToSQLite) {
				initializeTabularDataTable();
				initializeTabularDataView();
			}
		} else {
			throw std::runtime_error("The SQLite database failed to open.");
		}
	}
}

SQLite::~SQLite()
{
	sqlite3_finalize(m_reportDataInsertStmt);
	sqlite3_finalize(m_reportExtendedDataInsertStmt);
	sqlite3_finalize(m_reportDictionaryInsertStmt);
	sqlite3_finalize(m_timeIndexInsertStmt);
	sqlite3_finalize(m_zoneInfoInsertStmt);
	sqlite3_finalize(m_nominalLightingInsertStmt);
	sqlite3_finalize(m_nominalElectricEquipmentInsertStmt);
	sqlite3_finalize(m_nominalGasEquipmentInsertStmt);
	sqlite3_finalize(m_nominalSteamEquipmentInsertStmt);
	sqlite3_finalize(m_nominalHotWaterEquipmentInsertStmt);
	sqlite3_finalize(m_nominalOtherEquipmentInsertStmt);
	sqlite3_finalize(m_nominalBaseboardHeatInsertStmt);
	sqlite3_finalize(m_surfaceInsertStmt);
	sqlite3_finalize(m_constructionInsertStmt);
	sqlite3_finalize(m_constructionLayerInsertStmt);
	sqlite3_finalize(m_materialInsertStmt);
	sqlite3_finalize(m_zoneListInsertStmt);
	sqlite3_finalize(m_zoneGroupInsertStmt);
	sqlite3_finalize(m_infiltrationInsertStmt);
	sqlite3_finalize(m_ventilationInsertStmt);
	sqlite3_finalize(m_nominalPeopleInsertStmt);
	sqlite3_finalize(m_zoneSizingInsertStmt);
	sqlite3_finalize(m_systemSizingInsertStmt);
	sqlite3_finalize(m_componentSizingInsertStmt);
	sqlite3_finalize(m_roomAirModelInsertStmt);
	sqlite3_finalize(m_groundTemperatureInsertStmt);
	sqlite3_finalize(m_weatherFileInsertStmt);
	sqlite3_finalize(m_scheduleInsertStmt);
	sqlite3_finalize(m_daylightMapTitleInsertStmt);
	sqlite3_finalize(m_daylightMapHorlyTitleInsertStmt);
	sqlite3_finalize(m_daylightMapHorlyDataInsertStmt);
	sqlite3_finalize(m_environmentPeriodInsertStmt);
	sqlite3_finalize(m_simulationsInsertStmt);
	sqlite3_finalize(m_tabularDataInsertStmt);
	sqlite3_finalize(m_stringsInsertStmt);
	sqlite3_finalize(m_stringsLookUpStmt);
	sqlite3_finalize(m_errorInsertStmt);
	sqlite3_finalize(m_errorUpdateStmt);
	sqlite3_finalize(m_simulationUpdateStmt);

	sqlite3_close(m_db);
}

bool SQLite::writeOutputToSQLite() const
{
	return m_writeOutputToSQLite;
}

bool SQLite::writeTabularDataToSQLite() const
{
	return m_writeTabularDataToSQLite;
}

void SQLite::sqliteBegin()
{
	if( m_writeOutputToSQLite ) {
		sqliteExecuteCommand("BEGIN;");
	}
}

void SQLite::sqliteCommit()
{
	if( m_writeOutputToSQLite ) {
		sqliteExecuteCommand("COMMIT;");
	}
}

int SQLite::sqliteExecuteCommand(const std::string & commandBuffer)
{
	char *zErrMsg = 0;

	int rc = sqlite3_exec(m_db, commandBuffer.c_str(), NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ) {
		m_errorStream << zErrMsg;
	}
	sqlite3_free(zErrMsg);

	return rc;
}

int SQLite::sqlitePrepareStatement(sqlite3_stmt* & stmt, const std::string & stmtBuffer)
{
	int rc = sqlite3_prepare_v2(m_db, stmtBuffer.c_str(), -1, &stmt, nullptr);
	if( rc != SQLITE_OK ) {
		m_errorStream << "SQLite3 message, sqlite3_prepare_v2 message: " << stmtBuffer << std::endl;
	}

	return rc;
}

int SQLite::sqliteBindText(sqlite3_stmt * stmt, const int stmtInsertLocationIndex, const std::string & textBuffer)
{
	int rc = sqlite3_bind_text(stmt, stmtInsertLocationIndex, textBuffer.c_str(), -1, SQLITE_TRANSIENT);
	if( rc != SQLITE_OK ) {
		m_errorStream << "SQLite3 message, sqlite3_bind_text failed: " << textBuffer << std::endl;
	}

	return rc;
}

int SQLite::sqliteBindInteger(sqlite3_stmt * stmt, const int stmtInsertLocationIndex, const int intToInsert)
{
	int rc = sqlite3_bind_int(stmt, stmtInsertLocationIndex, intToInsert);
	if( rc != SQLITE_OK ) {
		m_errorStream << "SQLite3 message, sqlite3_bind_int failed: " << intToInsert << std::endl;
	}

	return rc;
}

int SQLite::sqliteBindDouble(sqlite3_stmt * stmt, const int stmtInsertLocationIndex, const double doubleToInsert)
{
	int rc = sqlite3_bind_double(stmt, stmtInsertLocationIndex, doubleToInsert);
	if( rc != SQLITE_OK ) {
		m_errorStream << "SQLite3 message, sqlite3_bind_double failed: " << doubleToInsert << std::endl;
	}

	return rc;
}

int SQLite::sqliteBindNULL(sqlite3_stmt * stmt, const int stmtInsertLocationIndex)
{
	int rc = sqlite3_bind_null(stmt, stmtInsertLocationIndex);
	if( rc != SQLITE_OK ) {
		m_errorStream << "SQLite3 message, sqlite3_bind_null failed" << std::endl;
	}

	return rc;
}

int SQLite::sqliteBindLogical(sqlite3_stmt * stmt, const int stmtInsertLocationIndex, const bool valueToInsert)
{
	return sqliteBindInteger(stmt,stmtInsertLocationIndex, valueToInsert ? 1 : 0);
}

int SQLite::sqliteStepCommand(sqlite3_stmt * stmt)
{
	int rc = sqlite3_step(stmt);
	switch(rc) {
	case SQLITE_DONE:
	case SQLITE_OK:
	case SQLITE_ROW:
		break;

	default:
		m_errorStream << "SQLite3 message, sqlite3_step message: " << sqlite3_errmsg(m_db) << std::endl;
		break;
	}

	return rc;
}

int SQLite::sqliteResetCommand(sqlite3_stmt * stmt)
{
	return sqlite3_reset(stmt);
}

int SQLite::sqliteClearBindings(sqlite3_stmt * stmt)
{
	return sqlite3_clear_bindings(stmt);
}

int SQLite::sqliteFinalizeCommand(sqlite3_stmt * stmt)
{
	return sqlite3_finalize(stmt);
}

void SQLite::sqliteWriteMessage(const std::string & message)
{
	if( m_writeOutputToSQLite ) {
		m_errorStream << "SQLite3 message, " << message << std::endl;
	}
}

void SQLite::initializeReportDataDictionaryTable()
{
	const std::string newTableSQL =
			"CREATE TABLE ReportDataDictionary("
					"ReportDataDictionaryIndex INTEGER PRIMARY KEY, "
					"IsMeter INTEGER, "
					"Type TEXT, "
					"IndexGroup TEXT, "
					"TimestepType TEXT, "
					"KeyValue TEXT, "
					"Name TEXT, "
					"ReportingFrequency TEXT, "
					"ScheduleName TEXT, "
					"Units TEXT);";

	sqliteExecuteCommand(newTableSQL);

	const std::string preparedSQL =
			"INSERT INTO ReportDataDictionary ("
					"ReportDataDictionaryIndex, "
					"IsMeter, "
					"Type, "
					"IndexGroup, "
					"TimestepType, "
					"KeyValue, "
					"Name, "
					"ReportingFrequency, "
					"ScheduleName, "
					"Units) "
					"VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_reportDictionaryInsertStmt,preparedSQL);
}

void SQLite::initializeReportDataTables()
{
	const std::string reportDataTableSQL =
			"CREATE TABLE ReportData ("
					"ReportDataIndex INTEGER PRIMARY KEY, "
					"TimeIndex INTEGER, "
					"ReportDataDictionaryIndex INTEGER, "
					"Value REAL);";

	sqliteExecuteCommand(reportDataTableSQL);

	const std::string reportDataInsertSQL =
			"INSERT INTO ReportData ("
					"ReportDataIndex, "
					"TimeIndex, "
					"ReportDataDictionaryIndex, "
					"Value) "
					"VALUES(?,?,?,?);";

	sqlitePrepareStatement(m_reportDataInsertStmt,reportDataInsertSQL);

	const std::string reportExtendedDataTableSQL =
			"CREATE TABLE ReportExtendedData ("
					"ReportExtendedDataIndex INTEGER PRIMARY KEY, "
					"ReportDataIndex INTEGER, "
					"MaxValue REAL, "
					"MaxMonth INTEGER, "
					"MaxDay INTEGER, "
					"MaxHour INTEGER, "
					"MaxStartMinute INTEGER, "
					"MaxMinute INTEGER, "
					"MinValue REAL, "
					"MinMonth INTEGER, "
					"MinDay INTEGER, "
					"MinHour INTEGER, "
					"MinStartMinute INTEGER, "
					"MinMinute INTEGER);";

	sqliteExecuteCommand(reportExtendedDataTableSQL);

	const std::string reportExtendedDataInsertSQL =
			"INSERT INTO ReportExtendedData ("
					"ReportExtendedDataIndex, "
					"ReportDataIndex, "
					"MaxValue, "
					"MaxMonth, "
					"MaxDay, "
					"MaxHour, "
					"MaxStartMinute, "
					"MaxMinute, "
					"MinValue, "
					"MinMonth, "
					"MinDay, "
					"MinHour, "
					"MinStartMinute, "
					"MinMinute) "
					"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_reportExtendedDataInsertStmt,reportExtendedDataInsertSQL);
}

void SQLite::initializeTimeIndicesTable()
{
	const std::string timeTableSQL =
		"CREATE TABLE Time ("
		"TimeIndex INTEGER PRIMARY KEY, "
		"Month INTEGER, "
		"Day INTEGER, "
		"Hour INTEGER, "
		"Minute INTEGER, "
		"Dst INTEGER, "
		"Interval INTEGER, "
		"IntervalType INTEGER, "
		"SimulationDays INTEGER, "
		"DayType TEXT, "
		"EnvironmentPeriodIndex INTEGER, "
		"WarmupFlag INTEGER);";

	sqliteExecuteCommand(timeTableSQL);

	const std::string timeIndexInsertSQL =
		"INSERT INTO Time ("
		"TimeIndex, "
		"Month, "
		"Day, "
		"Hour, "
		"Minute, "
		"DST, "
		"Interval, "
		"IntervalType, "
		"SimulationDays, "
		"DayType, "
		"EnvironmentPeriodIndex, "
		"WarmupFlag) "
		"VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_timeIndexInsertStmt,timeIndexInsertSQL);
}

void SQLite::initializeZoneInfoTable()
{
	const std::string zonesTableSQL =
		"CREATE TABLE Zones ("
		"ZoneIndex INTEGER PRIMARY KEY, "
		"ZoneName TEXT, "
		"RelNorth REAL, "
		"OriginX REAL, "
		"OriginY REAL, "
		"OriginZ REAL, "
		"CentroidX REAL, "
		"CentroidY REAL, "
		"CentroidZ REAL, "
		"OfType INTEGER, "
		"Multiplier REAL, "
		"ListMultiplier REAL, "
		"MinimumX REAL, "
		"MaximumX REAL, "
		"MinimumY REAL, "
		"MaximumY REAL, "
		"MinimumZ REAL, "
		"MaximumZ REAL, "
		"CeilingHeight REAL, "
		"Volume REAL, "
		"InsideConvectionAlgo INTEGER, "
		"OutsideConvectionAlgo INTEGER, "
		"FloorArea REAL, "
		"ExtGrossWallArea REAL, "
		"ExtNetWallArea REAL, "
		"ExtWindowArea REAL, "
		"IsPartOfTotalArea INTEGER);";

	sqliteExecuteCommand(zonesTableSQL);

	const std::string zoneInfoInsertSQL =
		"INSERT INTO Zones ("
		"ZoneIndex, "
		"ZoneName, "
		"RelNorth, "
		"OriginX, "
		"OriginY, "

		"OriginZ, "
		"CentroidX, "
		"CentroidY, "
		"CentroidZ, "
		"OfType, "

		"Multiplier, "
		"ListMultiplier, "
		"MinimumX, "
		"MaximumX, "
		"MinimumY, "

		"MaximumY, "
		"MinimumZ, "
		"MaximumZ, "
		"CeilingHeight, "
		"Volume, "

		"InsideConvectionAlgo, "
		"OutsideConvectionAlgo, "
		"FloorArea, "
		"ExtGrossWallArea, "
		"ExtNetWallArea, "

		"ExtWindowArea, "
		"IsPartOfTotalArea) "
		"VALUES (?,?,?,?,?, ?,?,?,?,?, ?,?,?,?,?, ?,?,?,?,?, ?,?,?,?,?, ?,?);";

	sqlitePrepareStatement(m_zoneInfoInsertStmt,zoneInfoInsertSQL);
}

void SQLite::initializeNominalPeopleTable()
{
	const std::string nominalPeopleTableSQL =
		"CREATE TABLE NominalPeople (NominalPeopleIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, ZoneIndex INTEGER,"
		"NumberOfPeople INTEGER, NumberOfPeopleScheduleIndex INTEGER, ActivityScheduleIndex INTEGER, FractionRadiant REAL, "
		"FractionConvected REAL, WorkEfficiencyScheduleIndex INTEGER, ClothingEfficiencyScheduleIndex INTEGER, "
		"AirVelocityScheduleIndex INTEGER, Fanger INTEGER, Pierce INTEGER, KSU INTEGER, "
		"MRTCalcType INTEGER, SurfaceIndex INTEGER, "
		"AngleFactorListName TEXT, AngleFactorList INTEGER, UserSpecifeidSensibleFraction REAL, Show55Warning INTEGER"
		");";

	sqliteExecuteCommand(nominalPeopleTableSQL);

	const std::string nominalPeopleInsertSQL =
		"INSERT INTO NominalPeople VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalPeopleInsertStmt,nominalPeopleInsertSQL);
}

void SQLite::initializeNominalLightingTable()
{
	const std::string nominalLightingTableSQL =
		"CREATE TABLE NominalLighting (NominalLightingIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL, FractionReturnAir REAL, FractionRadiant REAL, "
		"FractionShortWave REAL, FractionReplaceable REAL, FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalLightingTableSQL);

	const std::string nominalLightingInsertSQL =
		"INSERT INTO NominalLighting VALUES(?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalLightingInsertStmt,nominalLightingInsertSQL);
}

void SQLite::initializeNominalElectricEquipmentTable()
{
	const std::string nominalElectricEquipmentTableSQL =
		"CREATE TABLE NominalElectricEquipment ("
		"NominalElectricEquipmentIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL, "
		"FractionLatent REAL, FractionRadiant REAL, FractionLost REAL, "
		"FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalElectricEquipmentTableSQL);

	const std::string nominalElectricEquipmentInsertSQL =
		"INSERT INTO NominalElectricEquipment VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalElectricEquipmentInsertStmt,nominalElectricEquipmentInsertSQL);
}

void SQLite::initializeNominalGasEquipmentTable()
{
	const std::string nominalGasEquipmentTableSQL =
		"CREATE TABLE NominalGasEquipment(NominalGasEquipmentIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, "
		"DesignLevel REAL, FractionLatent REAL, FractionRadiant REAL, FractionLost REAL, "
		"FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalGasEquipmentTableSQL);

	const std::string nominalGasEquipmentInsertSQL =
		"INSERT INTO NominalGasEquipment VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalGasEquipmentInsertStmt,nominalGasEquipmentInsertSQL);
}

void SQLite::initializeNominalSteamEquipmentTable()
{
	const std::string nominalSteamEquipmentTableSQL =
		"CREATE TABLE NominalSteamEquipment(NominalSteamEquipmentIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL, "
		"FractionLatent REAL, FractionRadiant REAL, FractionLost REAL, "
		"FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalSteamEquipmentTableSQL);

	const std::string nominalSteamEquipmentInsertSQL =
		"INSERT INTO NominalSteamEquipment VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalSteamEquipmentInsertStmt,nominalSteamEquipmentInsertSQL);
}

void SQLite::initializeNominalHotWaterEquipmentTable()
{
	const std::string nominalHotWaterEquipmentTableSQL =
		"CREATE TABLE NominalHotWaterEquipment("
		"NominalHotWaterEquipmentIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, SchedNo INTEGER, DesignLevel REAL, FractionLatent REAL, FractionRadiant REAL, FractionLost REAL, "
		"FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalHotWaterEquipmentTableSQL);

	const std::string nominalHotWaterEquipmentInsertSQL =
		"INSERT INTO NominalHotWaterEquipment VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalHotWaterEquipmentInsertStmt,nominalHotWaterEquipmentInsertSQL);
}

void SQLite::initializeNominalOtherEquipmentTable()
{
	const std::string nominalOtherEquipmentTableSQL =
		"CREATE TABLE NominalOtherEquipment(NominalOtherEquipmentIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL, FractionLatent REAL, "
		"FractionRadiant REAL, FractionLost REAL, "
		"FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalOtherEquipmentTableSQL);

	const std::string nominalOtherEquipmentInsertSQL =
		"INSERT INTO NominalOtherEquipment VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalHotWaterEquipmentInsertStmt,nominalOtherEquipmentInsertSQL);
}

void SQLite::initializeNominalBaseboardHeatTable()
{
	const std::string nominalBaseboardHeatersTableSQL =
		"CREATE TABLE NominalBaseboardHeaters (NominalBaseboardHeaterIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, CapatLowTemperature REAL, LowTemperature REAL, CapatHighTemperature REAL, "
		"HighTemperature REAL, FractionRadiant REAL, FractionConvected REAL, EndUseSubcategory TEXT);";

	sqliteExecuteCommand(nominalBaseboardHeatersTableSQL);

	const std::string nominalBaseboardHeatInsertSQL =
		"INSERT INTO NominalBaseboardHeaters VALUES(?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_nominalBaseboardHeatInsertStmt,nominalBaseboardHeatInsertSQL);
}

void SQLite::initializeSurfacesTable()
{
	const std::string surfacesTableSQL =
		"CREATE TABLE Surfaces (SurfaceIndex INTEGER PRIMARY KEY, "
		"SurfaceName, ConstructionIndex INTEGER, "
		"ClassName TEXT, Area REAL, GrossArea REAL, Perimeter REAL, "
		"Azimuth REAL, Height REAL, Reveal REAL, "
		"Shape INTEGER, Sides INTEGER, Tilt REAL, Width REAL, HeatTransferSurf INTEGER, "
		"BaseSurfaceIndex INTEGER, ZoneIndex INTEGER, ExtBoundCond INTEGER,  "
		"ExtSolar INTEGER, ExtWind INTEGER"
		");";

	sqliteExecuteCommand(surfacesTableSQL);

	const std::string surfaceInsertSQL =
		"INSERT INTO Surfaces VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_surfaceInsertStmt,surfaceInsertSQL);
}

void SQLite::initializeConstructionsTables()
{
	const std::string constructionsTableSQL =
		"CREATE TABLE Constructions (ConstructionIndex INTEGER PRIMARY KEY, Name TEXT, TotalLayers INTEGER, "
		"TotalSolidLayers INTEGER, TotalGlassLayers INTEGER, InsideAbsorpVis REAL, OutsideAbsorpVis REAL,"
		" InsideAbsorpSolar REAL, OutsideAbsorpSolar REAL, InsideAbsorpThermal REAL, OutsideAbsorpThermal REAL, "
		"OutsideRoughness INTEGER, TypeIsWindow INTEGER, Uvalue REAL"
		");";

	sqliteExecuteCommand(constructionsTableSQL);

	const std::string constructionInsertSQL =
		"INSERT INTO Constructions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_constructionInsertStmt,constructionInsertSQL);

	const std::string constructionLayersTableSQL =
		"CREATE TABLE ConstructionLayers (ConstructionIndex INTEGER, "
		"LayerIndex INTEGER, MaterialIndex INTEGER);";

	sqliteExecuteCommand(constructionLayersTableSQL);

	const std::string constructionLayerInsertSQL =
		"INSERT INTO ConstructionLayers VALUES(?,?,?);";

	sqlitePrepareStatement(m_constructionLayerInsertStmt,constructionLayerInsertSQL);
}

void SQLite::initializeMaterialsTable()
{
	const std::string materialsTableSQL =
		"CREATE TABLE Materials (MaterialIndex INTEGER PRIMARY KEY, Name TEXT, MaterialType INTEGER, "
		"Roughness INTEGER, "
		"Conductivity REAL, Density REAL, IsoMoistCap REAL, Porosity REAL, Resistance REAL, "
		"ROnly INTEGER, SpecHeat REAL, ThermGradCoef REAL, Thickness REAL, VaporDiffus"
		");";

	sqliteExecuteCommand(materialsTableSQL);

	const std::string materialInsertSQL =
		"INSERT INTO Materials VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_materialInsertStmt,materialInsertSQL);
}

void SQLite::initializeZoneListTable()
{
	const std::string zoneListsTableSQL =
		"CREATE TABLE ZoneLists (ZoneListIndex INTEGER PRIMARY KEY, Name TEXT, "
		"ZoneIndex INTEGER);";

	sqliteExecuteCommand(zoneListsTableSQL);

	const std::string zoneListInsertSQL =
		"INSERT INTO ZoneLists VALUES(?,?,?);";

	sqlitePrepareStatement(m_zoneListInsertStmt,zoneListInsertSQL);
}

void SQLite::initializeZoneGroupTable()
{
	const std::string zoneGroupsTableSQL =
		"CREATE TABLE ZoneGroups (ZoneGroupIndex INTEGER PRIMARY KEY, ZoneListName TEXT, ZoneListMultiplier INTEGER);";

	sqliteExecuteCommand(zoneGroupsTableSQL);

	const std::string zoneGroupInsertSQL =
		"INSERT INTO ZoneGroups VALUES(?,?,?);";

	sqlitePrepareStatement(m_zoneGroupInsertStmt,zoneGroupInsertSQL);
}

void SQLite::initializeNominalInfiltrationTable()
{
	const std::string nominalInfiltrationTableSQL =
		"CREATE TABLE NominalInfiltration (NominalInfiltrationIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL);";

	sqliteExecuteCommand(nominalInfiltrationTableSQL);

	const std::string infiltrationInsertSQL =
		"INSERT INTO NominalInfiltration (NominalInfiltrationIndex, ObjectName, ZoneIndex, ScheduleIndex, DesignLevel)"
		"VALUES (?,?,?,?,?);";

	sqlitePrepareStatement(m_infiltrationInsertStmt,infiltrationInsertSQL);
}

void SQLite::initializeNominalVentilationTable()
{
	const std::string nominalVentilationTableSQL =
		"CREATE TABLE NominalVentilation (NominalVentilationIndex INTEGER PRIMARY KEY, "
		"ObjectName TEXT, "
		"ZoneIndex INTEGER, ScheduleIndex INTEGER, DesignLevel REAL);";

	sqliteExecuteCommand(nominalVentilationTableSQL);

	const std::string ventilationInsertSQL =
		"INSERT INTO NominalVentilation VALUES(?,?,?,?,?);";

	sqlitePrepareStatement(m_ventilationInsertStmt,ventilationInsertSQL);
}

void SQLite::initializeZoneSizingTable()
{
	const std::string zoneSizesTableSQL =
		"CREATE TABLE ZoneSizes (ZoneName TEXT, LoadType TEXT, "
		"CalcDesLoad REAL, UserDesLoad REAL, CalcDesFlow REAL, UserDesFlow REAL, DesDayName TEXT, PeakHrMin TEXT, "
		"PeakTemp REAL, PeakHumRat REAL, CalcOutsideAirFlow REAL"
		");";

	sqliteExecuteCommand(zoneSizesTableSQL);

	const std::string zoneSizingInsertSQL =
		"INSERT INTO ZoneSizes VALUES(?,?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_zoneSizingInsertStmt,zoneSizingInsertSQL);
}

void SQLite::initializeSystemSizingTable()
{
	const std::string systemSizesTableSQL =
		"CREATE TABLE SystemSizes (SystemName TEXT, Description TEXT, Value REAL, Units TEXT);";

	sqliteExecuteCommand(systemSizesTableSQL);

	const std::string systemSizingInsertSQL =
		"INSERT INTO SystemSizes VALUES(?,?,?,?);";

	sqlitePrepareStatement(m_systemSizingInsertStmt,systemSizingInsertSQL);
}

void SQLite::initializeComponentSizingTable()
{
	const std::string componentSizesTableSQL =
		"CREATE TABLE ComponentSizes (CompType TEXT, CompName TEXT, "
		"Description TEXT, Value REAL, Units TEXT);";

	sqliteExecuteCommand(componentSizesTableSQL);

	const std::string componentSizingInsertSQL =
		"INSERT INTO ComponentSizes VALUES (?,?,?,?,?);";

	sqlitePrepareStatement(m_componentSizingInsertStmt,componentSizingInsertSQL);
}

void SQLite::initializeRoomAirModelTable()
{
	const std::string roomAirModelsTableSQL =
		"CREATE TABLE RoomAirModels (ZoneIndex INTEGER PRIMARY KEY, AirModelName TEXT, AirModelType INTEGER, "
		"TempCoupleScheme INTEGER, SimAirModel INTEGER);";

	sqliteExecuteCommand(roomAirModelsTableSQL);

	const std::string roomAirModelInsertSQL =
		"INSERT INTO RoomAirModels VALUES(?,?,?,?,?);";

	sqlitePrepareStatement(m_roomAirModelInsertStmt,roomAirModelInsertSQL);
}

void SQLite::initializeSchedulesTable()
{
	const std::string scheduleTableSQL =
		"CREATE TABLE Schedules (ScheduleIndex INTEGER PRIMARY KEY, ScheduleName TEXT, "
		"ScheduleType TEXT, ScheduleMinimum REAL, ScheduleMaximum REAL);";

	sqliteExecuteCommand(scheduleTableSQL);

	const std::string scheduleInsertSQL =
		"INSERT INTO Schedules VALUES(?,?,?,?,?);";

	sqlitePrepareStatement(m_scheduleInsertStmt,scheduleInsertSQL);
}

void SQLite::initializeDaylightMapTables()
{
	const std::string daylightMapsTableSQL =
		"CREATE TABLE DaylightMaps (MapNumber INTEGER PRIMARY KEY, MapName TEXT, "
		"Environment TEXT, Zone INTEGER, ReferencePt1 TEXT, ReferencePt2 TEXT, Z REAL);";

	sqliteExecuteCommand(daylightMapsTableSQL);

	const std::string daylightMapTitleInsertSQL =
		"INSERT INTO DaylightMaps VALUES(?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_daylightMapHorlyTitleInsertStmt,daylightMapTitleInsertSQL);

	const std::string daylightMapHourlyReportsTableSQL =
		"CREATE TABLE DaylightMapHourlyReports (HourlyReportIndex INTEGER PRIMARY KEY, "
		"MapNumber INTEGER, Month INTEGER, DayOfMonth INTEGER, Hour INTEGER);";

	sqliteExecuteCommand(daylightMapHourlyReportsTableSQL);

	const std::string daylightMapHorlyTitleInsertSQL =
		"INSERT INTO DaylightMapHourlyReports VALUES(?,?,?,?,?);";

	sqlitePrepareStatement(m_daylightMapHorlyTitleInsertStmt,daylightMapHorlyTitleInsertSQL);

	const std::string daylightMapHourlyDataTableSQL =
		"CREATE TABLE DaylightMapHourlyData (HourlyReportIndex INTEGER, "
		"X REAL, Y REAL, Illuminance REAL);";

	sqliteExecuteCommand(daylightMapHourlyDataTableSQL);

	const std::string daylightMapHorlyDataInsertSQL =
		"INSERT INTO DaylightMapHourlyData VALUES(?,?,?,?);";

	sqlitePrepareStatement(m_daylightMapHorlyDataInsertStmt,daylightMapHorlyDataInsertSQL);
}

void SQLite::initializeViews()
{
	const std::string reportVariableWithTimeViewSQL =
		"CREATE VIEW ReportVariableWithTime "
		"AS "
		"SELECT ReportVariableData.*, Time.*, ReportVariableDataDictionary.*, ReportVariableExtendedData.* "
		"FROM "
		"ReportVariableData LEFT OUTER JOIN ReportVariableExtendedData "
		"INNER JOIN Time "
		"INNER JOIN ReportVariableDataDictionary "
		"ON "
		"(ReportVariableData.ReportVariableExtendedDataIndex "
		"= ReportVariableExtendedData.ReportVariableExtendedDataIndex) "
		"AND "
		"(ReportVariableData.TimeIndex = Time.TimeIndex) "
		"AND "
		"(ReportVariableDataDictionary.ReportVariableDataDictionaryIndex "
		"= ReportVariableData.ReportVariableDataDictionaryIndex);";

	sqliteExecuteCommand(reportVariableWithTimeViewSQL);
}

void SQLite::initializeSimulationsTable()
{
	const std::string simulationsTableSQL =
		"CREATE TABLE Simulations (SimulationIndex INTEGER PRIMARY KEY, "
		"EnergyPlusVersion TEXT, TimeStamp TEXT, NumTimestepsPerHour INTEGER, Completed BOOL, "
		"CompletedSuccessfully BOOL);";

	sqliteExecuteCommand(simulationsTableSQL);

	const std::string simulationsInsertSQL =
		"INSERT INTO Simulations "
		"VALUES(?,?,?,?,'FALSE','FALSE');";

	sqlitePrepareStatement(m_simulationsInsertStmt,simulationsInsertSQL);

	const std::string simulationUpdateSQL =
		"UPDATE Simulations SET "
		"Completed = ?, CompletedSuccessfully = ? "
		"WHERE SimulationIndex = (SELECT count(*) FROM Simulations)";

	sqlitePrepareStatement(m_simulationUpdateStmt,simulationUpdateSQL);
}

void SQLite::initializeErrorsTable()
{
	const std::string errorsTableSQL =
		"CREATE TABLE Errors (ErrorIndex INTEGER PRIMARY KEY, SimulationIndex INTEGER, "
		"ErrorType INTEGER, ErrorMessage TEXT, Count INTEGER);";

	sqliteExecuteCommand(errorsTableSQL);

	const std::string errorInsertSQL =
		"INSERT INTO Errors VALUES(?,?,?,?,?);";

	sqlitePrepareStatement(m_errorInsertStmt,errorInsertSQL);

	const std::string errorUpdateSQL =
		"UPDATE Errors SET "
		"ErrorMessage = ErrorMessage || ? WHERE ErrorIndex = (SELECT count(*) FROM Errors)";

	sqlitePrepareStatement(m_errorUpdateStmt,errorUpdateSQL);
}

void SQLite::initializeEnvironmentPeriodsTable()
{
	const std::string environmentPeriodsTableSQL =
		"CREATE TABLE EnvironmentPeriods (EnvironmentPeriodIndex INTEGER PRIMARY KEY, "
		"SimulationIndex INTEGER, EnvironmentName TEXT, EnvironmentType INTEGER);";

	sqliteExecuteCommand(environmentPeriodsTableSQL);

	const std::string environmentPeriodInsertSQL =
		"INSERT INTO EnvironmentPeriods VALUES(?,?,?,?);";

	sqlitePrepareStatement(m_environmentPeriodInsertStmt,environmentPeriodInsertSQL);
}

void SQLite::initializeTabularDataTable()
{
	const std::string sql = "CREATE TABLE TabularData "
							"(ReportNameIndex INTEGER, "
							"ReportForStringIndex INTEGER, "
							"TableNameIndex INTEGER, "
							"SimulationIndex INTEGER, "
							"RowNameIndex INTEGER, "
							"ColumnNameIndex INTEGER, "
							"RowId INTEGER, "
							"ColumnId INTEGER, "
							"Value TEXT, "
							"UnitsIndex INTEGER);";

	sqliteExecuteCommand(sql);

	const std::string sql2 = "INSERT INTO TabularData VALUES(?,?,?,?,?,?,?,?,?,?);";

	sqlitePrepareStatement(m_tabularDataInsertStmt,sql2);

	const std::string sql3 = "CREATE TABLE Strings "
							 "(StringIndex INTEGER PRIMARY KEY, "
							 "StringTypeIndex  INTEGER, "
							 "Value TEXT, "
							 "UNIQUE(StringTypeIndex, Value));";

	sqliteExecuteCommand(sql3);

	const std::string sql4 = "INSERT INTO Strings (StringIndex,StringTypeIndex,Value) VALUES(?,?,?);";

	sqlitePrepareStatement(m_stringsInsertStmt,sql4);

	const std::string sql5 = "SELECT StringIndex FROM Strings WHERE StringTypeIndex=? AND Value=?;";

	sqlitePrepareStatement(m_stringsLookUpStmt,sql5);

	const std::string sql6 = "CREATE TABLE StringTypes "
							 "(StringTypeIndex INTEGER PRIMARY KEY, "
							 "Value TEXT);";

	sqliteExecuteCommand(sql6);

	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(ReportNameId) + ",'ReportName');");
	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(ReportForStringId) + ",'ReportForString');");
	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(TableNameId) + ",'TableName');");
	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(RowNameId) + ",'RowName');");
	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(ColumnNameId) + ",'ColumnName');");
	sqliteExecuteCommand("INSERT INTO StringTypes VALUES(" + std::to_string(UnitsId) + ",'Units');");
}

void SQLite::initializeTabularDataView()
{
	const std::string sql = "CREATE VIEW TabularDataWithStrings AS SELECT "
							"td.Value Value, "
							"reportn.Value ReportName, "
							"fs.Value ReportForString, "
							"tn.Value TableName, "
							"rn.Value RowName, "
							"cn.Value ColumnName, "
							"u.Value Units, "
							"RowId "
							"FROM TabularData td "
							"INNER JOIN Strings reportn ON reportn.StringIndex=td.ReportNameIndex "
							"INNER JOIN Strings fs ON fs.StringIndex=td.ReportForStringIndex "
							"INNER JOIN Strings tn ON tn.StringIndex=td.TableNameIndex "
							"INNER JOIN Strings rn ON rn.StringIndex=td.RowNameIndex "
							"INNER JOIN Strings cn ON cn.StringIndex=td.ColumnNameIndex "
							"INNER JOIN Strings u ON u.StringIndex=td.UnitsIndex "
							"WHERE "
							"reportn.StringTypeIndex=1 AND "
							"fs.StringTypeIndex=2 AND "
							"tn.StringTypeIndex=3 AND "
							"rn.StringTypeIndex=4 AND "
							"cn.StringTypeIndex=5 AND "
							"u.StringTypeIndex=6;";

	sqliteExecuteCommand(sql);
}

void SQLite::initializeIndexes()
{
	if( m_writeOutputToSQLite ) {
		// sqliteExecuteCommand("CREATE INDEX rvdTI ON ReportVariableData (TimeIndex ASC);");
		// sqliteExecuteCommand("CREATE INDEX rvdDI ON ReportVariableData (ReportVariableDataDictionaryIndex ASC);");
		// sqliteExecuteCommand("CREATE INDEX rmdTI ON ReportMeterData (TimeIndex ASC);");
		// sqliteExecuteCommand("CREATE INDEX rmdDI ON ReportMeterData (ReportMeterDataDictionaryIndex ASC);");
//		sqliteExecuteCommand("CREATE INDEX tiTI ON Time (TimeIndex ASC);");
		// sqliteExecuteCommand("CREATE INDEX dmhdHRI ON DaylightMapHourlyData (HourlyReportIndex ASC);");
	}
}

std::string SQLite::storageType(const int storageTypeIndex)
{
	std::string result;

	switch(storageTypeIndex) {
	case 1:
		result = "Avg";
		break;
	case 2:
		result = "Sum";
		break;
	default:
		result = "Unknown!!!";
	}

	return result;
}

std::string SQLite::timestepTypeName(const int timestepType)
{
	std::string result;

	switch(timestepType) {
	case 1:
		result = "HVAC System";
		break;
	case 2:
		result = "Zone";
		break;
	default:
		result = "Unknown!!!";
	}

	return result;
}

std::string SQLite::reportingFreqName(const int reportingFreqIndex)
{
	std::string result;

	switch(reportingFreqIndex) {
	case LocalReportEach:
		result = "HVAC System Timestep";
		break;
	case LocalReportTimeStep:
		result = "Zone Timestep";
		break;
	case LocalReportHourly:
		result = "Hourly";
		break;
	case LocalReportDaily:
		result = "Daily";
		break;
	case LocalReportMonthly:
		result = "Monthly";
		break;
	case LocalReportSim:
		result = "Run Period";
		break;
	default:
		result = "Unknown!!!";
		break;
	}

	return result;
}

void SQLite::adjustReportingHourAndMinutes(int & hour, int & minutes)
{
	switch(minutes) {
	case 60:
		minutes = 0;
		break;
	default:
		--hour;
	}
}

void SQLite::parseUnitsAndDescription(const std::string & combinedString, std::string & units, std::string & description)
{
	std::size_t leftPos = combinedString.find("[");
	std::size_t rightPos = combinedString.find("]");

	if( (leftPos < rightPos) && (leftPos != std::string::npos) && (rightPos != std::string::npos) ) {
		units = combinedString.substr(leftPos + 1,rightPos - leftPos - 1);
		description = combinedString.substr(0,leftPos - 1);
	} else {
		units = "";
		description = combinedString;
	}
}

int SQLite::logicalToInteger(const bool value)
{
	return value ? 1 : 0;
}

void SQLite::createSQLiteReportDictionaryRecord (
		int const reportVariableReportID,
		int const storeTypeIndex,
		std::string const & indexGroup,
		std::string const & keyedValueString,
		std::string const & variableName,
		int const indexType,
		std::string const & units,
		int const reportingFreq,
		bool isMeter,
		Optional_string_const scheduleName
)
{
	if( m_writeOutputToSQLite ) {
		sqliteBindInteger(m_reportDictionaryInsertStmt, 1, reportVariableReportID);
		sqliteBindLogical(m_reportDictionaryInsertStmt, 2, isMeter);
		sqliteBindText(m_reportDictionaryInsertStmt, 3, storageType(storeTypeIndex));
		sqliteBindText(m_reportDictionaryInsertStmt, 4, indexGroup);
		sqliteBindText(m_reportDictionaryInsertStmt, 5, timestepTypeName(indexType));
		sqliteBindText(m_reportDictionaryInsertStmt, 6, keyedValueString);
		sqliteBindText(m_reportDictionaryInsertStmt, 7, variableName);
		sqliteBindText(m_reportDictionaryInsertStmt, 8, reportingFreqName(reportingFreq));

		if(scheduleName.present()) {
			sqliteBindText(m_reportDictionaryInsertStmt, 9, scheduleName());
		} else {
			sqliteBindNULL(m_reportDictionaryInsertStmt, 9);
		}

		sqliteBindText(m_reportDictionaryInsertStmt, 10, units);

		sqliteStepCommand(m_reportDictionaryInsertStmt);
		sqliteResetCommand(m_reportDictionaryInsertStmt);
	}
}

void SQLite::createSQLiteReportDataRecord(
		int const recordIndex,
		Real64 const value,
		Optional_int_const reportingInterval,
		Optional< Real64 const > minValue,
		Optional_int_const minValueDate,
		Optional< Real64 const > maxValue,
		Optional_int_const maxValueDate,
		Optional_int_const minutesPerTimeStep
)
{
	if( m_writeOutputToSQLite ) {
		static int dataIndex = 0;
		static int extendedDataIndex = 0;

		++dataIndex;

		sqliteBindInteger(m_reportDataInsertStmt, 1, dataIndex);
		sqliteBindInteger(m_reportDataInsertStmt, 2, m_sqlDBTimeIndex);
		sqliteBindInteger(m_reportDataInsertStmt, 3, recordIndex);
		sqliteBindDouble(m_reportDataInsertStmt, 4, value);

		if(reportingInterval.present() && minValueDate != 0 && maxValueDate != 0) {
			int minMonth;
			int minDay;
			int minHour;
			int minMinute;
			int maxMonth;
			int maxDay;
			int maxHour;
			int maxMinute;

			General::DecodeMonDayHrMin(minValueDate, minMonth, minDay, minHour, minMinute);
			General::DecodeMonDayHrMin(maxValueDate, maxMonth, maxDay, maxHour, maxMinute);

			adjustReportingHourAndMinutes(minHour, minMinute);
			adjustReportingHourAndMinutes(maxHour, maxMinute);

			++extendedDataIndex;

			if(minutesPerTimeStep.present()) { // This is for data created by a 'Report Meter' statement
				switch(reportingInterval()) {
					case LocalReportHourly:
					case LocalReportDaily:
					case LocalReportMonthly:
					case LocalReportSim:
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 1, extendedDataIndex);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 2, dataIndex);

						sqliteBindDouble(m_reportExtendedDataInsertStmt, 3, maxValue);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 4, maxMonth);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 5, maxDay);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 6, maxHour);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 7, maxMinute - minutesPerTimeStep + 1);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 8, maxMinute);

						sqliteBindDouble(m_reportExtendedDataInsertStmt, 9, minValue);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 10, minMonth);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 11, minDay);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 12, minHour);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 13, minMinute - minutesPerTimeStep + 1);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 14, minMinute);

						sqliteStepCommand(m_reportExtendedDataInsertStmt);
						sqliteResetCommand(m_reportExtendedDataInsertStmt);
						break;

					case LocalReportTimeStep:
						--extendedDataIndex; // Reset the data index to account for the error
						break;

					default:
						--extendedDataIndex; // Reset the data index to account for the error
						std::stringstream ss;
						ss << "Illegal reportingInterval passed to CreateSQLiteMeterRecord: " << reportingInterval;
						sqliteWriteMessage(ss.str());
				}
			} else { // This is for data created by a 'Report Variable' statement
				switch(reportingInterval()) {
					case LocalReportDaily:
					case LocalReportMonthly:
					case LocalReportSim:
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 1, extendedDataIndex);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 2, dataIndex);

						sqliteBindDouble(m_reportExtendedDataInsertStmt, 3, maxValue);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 4, maxMonth);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 5, maxDay);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 6, maxHour);
						sqliteBindNULL(m_reportExtendedDataInsertStmt, 7);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 8, maxMinute);

						sqliteBindDouble(m_reportExtendedDataInsertStmt, 9, minValue);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 10, minMonth);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 11, minDay);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 12, minHour);
						sqliteBindNULL(m_reportExtendedDataInsertStmt, 13);
						sqliteBindInteger(m_reportExtendedDataInsertStmt, 14, minMinute);

						sqliteStepCommand(m_reportExtendedDataInsertStmt);
						sqliteResetCommand(m_reportExtendedDataInsertStmt);
						break;

					default:
						--extendedDataIndex; // Reset the data index to account for the error
						std::stringstream ss;
						ss << "Illegal reportingInterval passed to CreateSQLiteMeterRecord: " << reportingInterval;
						sqliteWriteMessage(ss.str());
				}
			}
		}

		sqliteStepCommand(m_reportDataInsertStmt);
		sqliteResetCommand(m_reportDataInsertStmt);
	}
}

void SQLite::createSQLiteTimeIndexRecord(
	int const reportingInterval,
	int const recordIndex,
	int const cumlativeSimulationDays,
	Optional_int_const month,
	Optional_int_const dayOfMonth,
	Optional_int_const hour,
	Optional< Real64 const > endMinute,
	Optional< Real64 const > startMinute,
	Optional_int_const dst,
	Optional_string_const dayType
)
{
	if( m_writeOutputToSQLite ) {
		int intStartMinute = 0;
		int intervalInMinutes = 60;

		static const std::vector<int> lastDayOfMonth = {31,28,31,30,31,30,31,31,30,31,30,31};

		switch(reportingInterval) {
		case LocalReportEach:
		case LocalReportTimeStep: {
			++m_sqlDBTimeIndex;

			int intEndMinute = static_cast<int>(endMinute() + 0.5);
			intStartMinute = static_cast<int>(startMinute() + 0.5);
			int t_hour = hour();
			intervalInMinutes = intEndMinute - intStartMinute;
			adjustReportingHourAndMinutes(t_hour, intEndMinute);

			sqliteBindInteger(m_timeIndexInsertStmt, 1, m_sqlDBTimeIndex);
			sqliteBindInteger(m_timeIndexInsertStmt, 2, month());
			sqliteBindInteger(m_timeIndexInsertStmt, 3, dayOfMonth());
			sqliteBindInteger(m_timeIndexInsertStmt, 4, t_hour);
			sqliteBindInteger(m_timeIndexInsertStmt, 5, intEndMinute);
			sqliteBindInteger(m_timeIndexInsertStmt, 6, dst());
			sqliteBindInteger(m_timeIndexInsertStmt, 7, intervalInMinutes);
			sqliteBindInteger(m_timeIndexInsertStmt, 8, reportingInterval);
			sqliteBindInteger(m_timeIndexInsertStmt, 9, cumlativeSimulationDays);
			sqliteBindText(m_timeIndexInsertStmt, 10, dayType());
			sqliteBindInteger(m_timeIndexInsertStmt, 11, DataEnvironment::CurEnvirNum);
			sqliteBindLogical(m_timeIndexInsertStmt, 12, DataGlobals::WarmupFlag);

			sqliteStepCommand(m_timeIndexInsertStmt);
			sqliteResetCommand(m_timeIndexInsertStmt);

			break;
		}
		case LocalReportHourly: {
			++m_sqlDBTimeIndex;

			sqliteBindInteger(m_timeIndexInsertStmt, 1, m_sqlDBTimeIndex);
			sqliteBindInteger(m_timeIndexInsertStmt, 2, month());
			sqliteBindInteger(m_timeIndexInsertStmt, 3, dayOfMonth());
			sqliteBindInteger(m_timeIndexInsertStmt, 4, hour());
			sqliteBindInteger(m_timeIndexInsertStmt, 5, 0);
			sqliteBindInteger(m_timeIndexInsertStmt, 6, dst());
			sqliteBindInteger(m_timeIndexInsertStmt, 7, intervalInMinutes);
			sqliteBindInteger(m_timeIndexInsertStmt, 8, reportingInterval);
			sqliteBindInteger(m_timeIndexInsertStmt, 9, cumlativeSimulationDays);
			sqliteBindText(m_timeIndexInsertStmt, 10, dayType());
			sqliteBindInteger(m_timeIndexInsertStmt, 11, DataEnvironment::CurEnvirNum);

			sqliteStepCommand(m_timeIndexInsertStmt);
			sqliteResetCommand(m_timeIndexInsertStmt);

			break;
		}
		case LocalReportDaily: {
			++m_sqlDBTimeIndex;

			intervalInMinutes = 60*24;
			sqliteBindInteger(m_timeIndexInsertStmt, 1, m_sqlDBTimeIndex);
			sqliteBindInteger(m_timeIndexInsertStmt, 2, month());
			sqliteBindInteger(m_timeIndexInsertStmt, 3, dayOfMonth());
			sqliteBindInteger(m_timeIndexInsertStmt, 4, 24);
			sqliteBindInteger(m_timeIndexInsertStmt, 5, 0);
			sqliteBindInteger(m_timeIndexInsertStmt, 6, dst());
			sqliteBindInteger(m_timeIndexInsertStmt, 7, intervalInMinutes);
			sqliteBindInteger(m_timeIndexInsertStmt, 8, reportingInterval);
			sqliteBindInteger(m_timeIndexInsertStmt, 9, cumlativeSimulationDays);
			sqliteBindText(m_timeIndexInsertStmt, 10, dayType());
			sqliteBindInteger(m_timeIndexInsertStmt, 11, DataEnvironment::CurEnvirNum);

			sqliteStepCommand(m_timeIndexInsertStmt);
			sqliteResetCommand(m_timeIndexInsertStmt);

			break;
		}
		case LocalReportMonthly: {
			++m_sqlDBTimeIndex;

			intervalInMinutes = 60*24*lastDayOfMonth[month()];
			sqliteBindInteger(m_timeIndexInsertStmt, 1, m_sqlDBTimeIndex);
			sqliteBindInteger(m_timeIndexInsertStmt, 2, month());
			sqliteBindInteger(m_timeIndexInsertStmt, 3, lastDayOfMonth[month()]);
			sqliteBindInteger(m_timeIndexInsertStmt, 4, 24);
			sqliteBindInteger(m_timeIndexInsertStmt, 5, 0);
			sqliteBindNULL(m_timeIndexInsertStmt, 6);
			sqliteBindInteger(m_timeIndexInsertStmt, 7, intervalInMinutes);
			sqliteBindInteger(m_timeIndexInsertStmt, 8, reportingInterval);
			sqliteBindInteger(m_timeIndexInsertStmt, 9, cumlativeSimulationDays);
			sqliteBindNULL(m_timeIndexInsertStmt, 10);
			sqliteBindInteger(m_timeIndexInsertStmt, 11, DataEnvironment::CurEnvirNum);

			sqliteStepCommand(m_timeIndexInsertStmt);
			sqliteResetCommand(m_timeIndexInsertStmt);

			break;
		}
		case LocalReportSim: {
			++m_sqlDBTimeIndex;

			intervalInMinutes = 60*24*cumlativeSimulationDays;
			sqliteBindInteger(m_timeIndexInsertStmt, 1, m_sqlDBTimeIndex);
			sqliteBindNULL(m_timeIndexInsertStmt, 2);
			sqliteBindNULL(m_timeIndexInsertStmt, 3);
			sqliteBindNULL(m_timeIndexInsertStmt, 4);
			sqliteBindNULL(m_timeIndexInsertStmt, 5);
			sqliteBindNULL(m_timeIndexInsertStmt, 6);
			sqliteBindInteger(m_timeIndexInsertStmt, 7, intervalInMinutes);
			sqliteBindInteger(m_timeIndexInsertStmt, 8, reportingInterval);
			sqliteBindInteger(m_timeIndexInsertStmt, 9, cumlativeSimulationDays);
			sqliteBindNULL(m_timeIndexInsertStmt, 10);
			sqliteBindInteger(m_timeIndexInsertStmt, 11, DataEnvironment::CurEnvirNum);

			sqliteStepCommand (m_timeIndexInsertStmt);
			sqliteResetCommand (m_timeIndexInsertStmt);

			break;
		}
		default: {
			std::stringstream ss;
			ss << "Illegal reportingInterval passed to CreateSQLiteTimeIndexRecord: " << reportingInterval;
			sqliteWriteMessage(ss.str());
		}
		}
	}
}

void SQLite::addSQLiteZoneSizingRecord(
	std::string const & zoneName, // the name of the zone
	std::string const & loadType, // the description of the input variable
	Real64 const calcDesLoad, // the value from the sizing calculation [W]
	Real64 const userDesLoad, // the value from the sizing calculation modified by user input [W]
	Real64 const calcDesFlow, // calculated design air flow rate [m3/s]
	Real64 const userDesFlow, // user input or modified design air flow rate [m3/s]
	std::string const & desDayName, // the name of the design day that produced the peak
	std::string const & peakHrMin, // time stamp of the peak
	Real64 const peakTemp, // temperature at peak [C]
	Real64 const peakHumRat, // humidity ratio at peak [kg water/kg dry air]
	Real64 const minOAVolFlow // zone design minimum outside air flow rate [m3/s]
)
{
	if( m_writeOutputToSQLite ) {
		sqliteBindText(m_zoneSizingInsertStmt, 1, zoneName);
		sqliteBindText(m_zoneSizingInsertStmt, 2, loadType);

		sqliteBindDouble(m_zoneSizingInsertStmt, 3, calcDesLoad);
		sqliteBindDouble(m_zoneSizingInsertStmt, 4, userDesLoad);
		sqliteBindDouble(m_zoneSizingInsertStmt, 5, calcDesFlow);
		sqliteBindDouble(m_zoneSizingInsertStmt, 6, userDesFlow);

		sqliteBindText(m_zoneSizingInsertStmt, 7, desDayName);
		sqliteBindText(m_zoneSizingInsertStmt, 8, peakHrMin);

		sqliteBindDouble(m_zoneSizingInsertStmt, 9, peakTemp);
		sqliteBindDouble(m_zoneSizingInsertStmt, 10, peakHumRat);
		sqliteBindDouble(m_zoneSizingInsertStmt, 11, minOAVolFlow);

		sqliteStepCommand(m_zoneSizingInsertStmt);
		sqliteResetCommand(m_zoneSizingInsertStmt);
	}
}

void SQLite::addSQLiteSystemSizingRecord(
	std::string const & sysName, // the name of the system
	std::string const & varDesc, // the description of the input variable
	Real64 const varValue // the value from the sizing calculation
)
{
	if( m_writeOutputToSQLite ) {
		std::string description;
		std::string units;

		parseUnitsAndDescription(varDesc,units,description);

		sqliteBindText(m_systemSizingInsertStmt, 1, sysName);
		sqliteBindText(m_systemSizingInsertStmt, 2, description);
		sqliteBindDouble(m_systemSizingInsertStmt, 3, varValue);
		sqliteBindText(m_systemSizingInsertStmt, 4, units);

		sqliteStepCommand(m_systemSizingInsertStmt);
		sqliteResetCommand(m_systemSizingInsertStmt);
	}
}

void SQLite::addSQLiteComponentSizingRecord(
	std::string const & compType, // the type of the component
	std::string const & compName, // the name of the component
	std::string const & varDesc, // the description of the input variable
	Real64 const varValue // the value from the sizing calculation
)
{
	if( m_writeOutputToSQLite ) {
		std::string description;
		std::string units;

		parseUnitsAndDescription(varDesc,units,description);

		sqliteBindText(m_componentSizingInsertStmt, 1, compType);
		sqliteBindText(m_componentSizingInsertStmt, 2, compName);
		sqliteBindText(m_componentSizingInsertStmt, 3, description);
		sqliteBindDouble(m_componentSizingInsertStmt, 4, varValue);
		sqliteBindText(m_componentSizingInsertStmt, 5, units);

		sqliteStepCommand(m_componentSizingInsertStmt);
		sqliteResetCommand(m_componentSizingInsertStmt);
	}
}

void SQLite::createSQLiteRoomAirModelTable()
{
	if( m_writeOutputToSQLite ) {
		for(int zoneNum = 1; zoneNum <= DataGlobals::NumOfZones; ++zoneNum) {
			auto& zoneAirModel = DataRoomAirModel::AirModel(zoneNum);
			sqliteBindInteger(m_roomAirModelInsertStmt, 1, zoneNum);
			sqliteBindText(m_roomAirModelInsertStmt, 2, zoneAirModel.AirModelName);
			sqliteBindInteger(m_roomAirModelInsertStmt, 3, zoneAirModel.AirModelType);
			sqliteBindInteger(m_roomAirModelInsertStmt, 4, zoneAirModel.TempCoupleScheme);
			sqliteBindLogical(m_roomAirModelInsertStmt, 5, zoneAirModel.SimAirModel);

			sqliteStepCommand(m_roomAirModelInsertStmt);
			sqliteResetCommand(m_roomAirModelInsertStmt);
		}
	}
}

void SQLite::createSQLiteDaylightMapTitle(
	int const mapNum,
	std::string const & mapName,
	std::string const & environmentName,
	int const zone,
	std::string const & refPt1,
	std::string const & refPt2,
	Real64 const zCoord
)
{
	if( m_writeOutputToSQLite ) {
		sqliteBindInteger(m_daylightMapTitleInsertStmt, 1, mapNum);
		sqliteBindText(m_daylightMapTitleInsertStmt, 2, mapName);
		sqliteBindText(m_daylightMapTitleInsertStmt, 3, environmentName);
		sqliteBindInteger(m_daylightMapTitleInsertStmt, 4, zone);
		sqliteBindText(m_daylightMapTitleInsertStmt, 5, refPt1);
		sqliteBindText(m_daylightMapTitleInsertStmt, 6, refPt2);
		sqliteBindDouble(m_daylightMapTitleInsertStmt, 7, zCoord);

		sqliteStepCommand(m_daylightMapTitleInsertStmt);
		sqliteResetCommand(m_daylightMapTitleInsertStmt);
	}
}

void SQLite::createSQLiteDaylightMap(
	int const mapNum,
	int const month,
	int const dayOfMonth,
	int const hourOfDay,
	int const nX,
	FArray1S< Real64 > const x,
	int const nY,
	FArray1S< Real64 > const y,
	FArray2S< Real64 > const illuminance
)
{
	if( m_writeOutputToSQLite ) {
		static int hourlyReportIndex = 1;

		sqliteBindInteger(m_daylightMapHorlyTitleInsertStmt, 1, hourlyReportIndex);
		sqliteBindInteger(m_daylightMapHorlyTitleInsertStmt, 2, mapNum);
		sqliteBindInteger(m_daylightMapHorlyTitleInsertStmt, 3, month);
		sqliteBindInteger(m_daylightMapHorlyTitleInsertStmt, 4, dayOfMonth);
		sqliteBindInteger(m_daylightMapHorlyTitleInsertStmt, 5, hourOfDay);

		sqliteStepCommand(m_daylightMapHorlyTitleInsertStmt);
		sqliteResetCommand(m_daylightMapHorlyTitleInsertStmt);

		for(int yIndex = 1; yIndex <= nY; ++yIndex) {
			for(int xIndex = 1; xIndex <= nX; ++xIndex) {
				sqliteBindInteger(m_daylightMapHorlyDataInsertStmt, 1, hourlyReportIndex);
				sqliteBindDouble(m_daylightMapHorlyDataInsertStmt, 2, x(xIndex));
				sqliteBindDouble(m_daylightMapHorlyDataInsertStmt, 3, y(yIndex));
				sqliteBindDouble(m_daylightMapHorlyDataInsertStmt, 4, illuminance(xIndex, yIndex));

				sqliteStepCommand(m_daylightMapHorlyDataInsertStmt);
				sqliteResetCommand(m_daylightMapHorlyDataInsertStmt);
			}
		}

		++hourlyReportIndex;
	}
}

void SQLite::createSQLiteTabularDataRecords(
	FArray2D_string const & body, // row,column
	FArray1D_string const & rowLabels,
	FArray1D_string const & columnLabels,
	std::string const & reportName,
	std::string const & reportForString,
	std::string const & tableName
)
{
	if(m_writeTabularDataToSQLite) {
		size_t sizeColumnLabels = columnLabels.size();
		size_t sizeRowLabels = rowLabels.size();

		for(size_t iRow = 0, k = body.index(1,1); iRow < sizeRowLabels; ++iRow) {
			std::string rowLabel = rowLabels[iRow];
			std::string rowUnits;
			std::string rowDescription;
			parseUnitsAndDescription(rowLabel,rowUnits,rowDescription);

			for(size_t iCol = 0; iCol < sizeColumnLabels; ++iCol) {
				std::string colLabel = columnLabels[iCol];
				std::string colUnits;
				std::string colDescription;
				parseUnitsAndDescription(colLabel,colUnits,colDescription);

				std::string units;
				if( colUnits.empty() ) {
					units = rowUnits;
				} else {
					units = colUnits;
				}

				int reportNameIndex = createSQLiteStringTableRecord(reportName,ReportNameId);
				int reportForStringIndex = createSQLiteStringTableRecord(reportForString,ReportForStringId);
				int tableNameIndex = createSQLiteStringTableRecord(tableName,TableNameId);
				int rowLabelIndex = createSQLiteStringTableRecord(rowDescription,RowNameId);
				int columnLabelIndex = createSQLiteStringTableRecord(colDescription,ColumnNameId);
				int unitsIndex = createSQLiteStringTableRecord(units,UnitsId);

				sqliteBindInteger(m_tabularDataInsertStmt,1,reportNameIndex);
				sqliteBindInteger(m_tabularDataInsertStmt,2,reportForStringIndex);
				sqliteBindInteger(m_tabularDataInsertStmt,3,tableNameIndex);
				sqliteBindInteger(m_tabularDataInsertStmt,4,1);
				sqliteBindInteger(m_tabularDataInsertStmt,5,rowLabelIndex);
				sqliteBindInteger(m_tabularDataInsertStmt,6,columnLabelIndex);
				sqliteBindInteger(m_tabularDataInsertStmt,7,iRow);
				sqliteBindInteger(m_tabularDataInsertStmt,8,iCol);
				sqliteBindText(m_tabularDataInsertStmt,9,body[k]);
				sqliteBindInteger(m_tabularDataInsertStmt,10,unitsIndex);

				++k;

				sqliteStepCommand(m_tabularDataInsertStmt);
				sqliteResetCommand(m_tabularDataInsertStmt);
				sqliteClearBindings(m_tabularDataInsertStmt);
			}
		}
	}
}

int SQLite::createSQLiteStringTableRecord(std::string const & stringValue, int const stringType)
{
	static int stringIndex = 1;
	if( m_writeOutputToSQLite ) {
		sqliteBindInteger(m_stringsInsertStmt, 1, stringIndex);
		sqliteBindInteger(m_stringsInsertStmt, 2, stringType);
		sqliteBindText(m_stringsInsertStmt, 3, stringValue);

		int errorcode = sqliteStepCommand(m_stringsInsertStmt);
		sqliteResetCommand(m_stringsInsertStmt);

		if( errorcode != SQLITE_CONSTRAINT ) {
			stringIndex++;
		}
	}
	return stringIndex;
}

void SQLite::createSQLiteSimulationsRecord( int const id )
{
	if( m_writeOutputToSQLite ) {
		sqliteBindInteger(m_simulationsInsertStmt, 1, id);
		sqliteBindText(m_simulationsInsertStmt, 2, DataStringGlobals::VerString);
		sqliteBindText(m_simulationsInsertStmt, 3, DataStringGlobals::CurrentDateTime);
		sqliteBindInteger(m_simulationsInsertStmt, 4, DataGlobals::NumOfTimeStepInHour);

		sqliteStepCommand(m_simulationsInsertStmt);
		sqliteResetCommand(m_simulationsInsertStmt);
	}
}

void SQLite::createSQLiteErrorRecord(
	int const simulationIndex,
	int const errorType,
	std::string const & errorMessage,
	int const cnt
)
{
	if( m_writeOutputToSQLite ) {
		static int errorIndex = 0;

		++errorIndex;

		sqliteBindInteger(m_errorInsertStmt, 1, errorIndex);
		sqliteBindInteger(m_errorInsertStmt, 2, simulationIndex);
		sqliteBindInteger(m_errorInsertStmt, 3, errorType);
		sqliteBindText(m_errorInsertStmt, 4, errorMessage);
		sqliteBindInteger(m_errorInsertStmt, 5, cnt);

		sqliteStepCommand(m_errorInsertStmt);
		sqliteResetCommand(m_errorInsertStmt);
	}
}

void SQLite::updateSQLiteErrorRecord( std::string const & errorMessage )
{
	if( m_writeOutputToSQLite ) {
		sqliteBindText(m_errorUpdateStmt, 1, "  " + errorMessage);

		sqliteStepCommand(m_errorUpdateStmt);
		sqliteResetCommand(m_errorUpdateStmt);
	}
}

void SQLite::updateSQLiteSimulationRecord(
	bool const completed,
	bool const completedSuccessfully
)
{
	if( m_writeOutputToSQLite ) {
		sqliteBindLogical(m_simulationUpdateStmt, 1, completed);
		sqliteBindLogical(m_simulationUpdateStmt, 2, completedSuccessfully);

		sqliteStepCommand(m_simulationUpdateStmt);
		sqliteResetCommand(m_simulationUpdateStmt);
	}
}

void SQLite::createSQLiteZoneTable()
{
	for( int zoneNum = 1; zoneNum <= DataGlobals::NumOfZones; ++zoneNum) {
		auto& zoneHB = DataHeatBalance::Zone(zoneNum);

		sqliteBindInteger(m_zoneInfoInsertStmt, 1, zoneNum);
		sqliteBindText(m_zoneInfoInsertStmt, 2, zoneHB.Name);
		sqliteBindDouble(m_zoneInfoInsertStmt, 3, zoneHB.RelNorth);
		sqliteBindDouble(m_zoneInfoInsertStmt, 4, zoneHB.OriginX);
		sqliteBindDouble(m_zoneInfoInsertStmt, 5, zoneHB.OriginY);
		sqliteBindDouble(m_zoneInfoInsertStmt, 6, zoneHB.OriginZ);
		sqliteBindDouble(m_zoneInfoInsertStmt, 7, zoneHB.Centroid.x);
		sqliteBindDouble(m_zoneInfoInsertStmt, 8, zoneHB.Centroid.y);
		sqliteBindDouble(m_zoneInfoInsertStmt, 9, zoneHB.Centroid.z);
		sqliteBindInteger(m_zoneInfoInsertStmt, 10, zoneHB.OfType);
		sqliteBindInteger(m_zoneInfoInsertStmt, 11, zoneHB.Multiplier);
		sqliteBindInteger(m_zoneInfoInsertStmt, 12, zoneHB.ListMultiplier);
		sqliteBindDouble(m_zoneInfoInsertStmt, 13, zoneHB.MinimumX);
		sqliteBindDouble(m_zoneInfoInsertStmt, 14, zoneHB.MaximumX);
		sqliteBindDouble(m_zoneInfoInsertStmt, 15, zoneHB.MinimumY);
		sqliteBindDouble(m_zoneInfoInsertStmt, 16, zoneHB.MaximumY);
		sqliteBindDouble(m_zoneInfoInsertStmt, 17, zoneHB.MinimumZ);
		sqliteBindDouble(m_zoneInfoInsertStmt, 18, zoneHB.MaximumZ);
		sqliteBindDouble(m_zoneInfoInsertStmt, 19, zoneHB.CeilingHeight);
		sqliteBindDouble(m_zoneInfoInsertStmt, 20, zoneHB.Volume);
		sqliteBindInteger(m_zoneInfoInsertStmt, 21, zoneHB.InsideConvectionAlgo);
		sqliteBindInteger(m_zoneInfoInsertStmt, 22, zoneHB.OutsideConvectionAlgo);
		sqliteBindDouble(m_zoneInfoInsertStmt, 23, zoneHB.FloorArea);
		sqliteBindDouble(m_zoneInfoInsertStmt, 24, zoneHB.ExtGrossWallArea);
		sqliteBindDouble(m_zoneInfoInsertStmt, 25, zoneHB.ExtNetWallArea);
		sqliteBindDouble(m_zoneInfoInsertStmt, 26, zoneHB.ExtWindowArea);
		sqliteBindLogical(m_zoneInfoInsertStmt, 27, zoneHB.isPartOfTotalArea);
		sqliteBindInteger(m_zoneInfoInsertStmt, 27, zoneHB.isPartOfTotalArea);

		sqliteStepCommand(m_zoneInfoInsertStmt);
		sqliteResetCommand(m_zoneInfoInsertStmt);
	}
}

void SQLite::createSQLiteNominalLightingTable()
{
	for(int lightNum = 1; lightNum <= DataHeatBalance::TotLights; ++lightNum) {
		auto& lightsHB = DataHeatBalance::Lights(lightNum);
		sqliteBindInteger(m_nominalLightingInsertStmt, 1, lightNum);
		sqliteBindText(m_nominalLightingInsertStmt, 2, lightsHB.Name);
		sqliteBindInteger(m_nominalLightingInsertStmt, 3, lightsHB.ZonePtr);
		sqliteBindInteger(m_nominalLightingInsertStmt, 4, lightsHB.SchedPtr);
		sqliteBindDouble(m_nominalLightingInsertStmt, 5, lightsHB.DesignLevel);
		sqliteBindDouble(m_nominalLightingInsertStmt, 6, lightsHB.FractionReturnAir);
		sqliteBindDouble(m_nominalLightingInsertStmt, 7, lightsHB.FractionRadiant);
		sqliteBindDouble(m_nominalLightingInsertStmt, 8, lightsHB.FractionShortWave);
		sqliteBindDouble(m_nominalLightingInsertStmt, 9, lightsHB.FractionReplaceable);
		sqliteBindDouble(m_nominalLightingInsertStmt, 10, lightsHB.FractionConvected);
		sqliteBindText(m_nominalLightingInsertStmt, 11, lightsHB.EndUseSubcategory);

		sqliteStepCommand(m_nominalLightingInsertStmt);
		sqliteResetCommand(m_nominalLightingInsertStmt);
	}
}

void SQLite::createSQLiteNominalPeopleTable()
{
	for(int peopleNum = 1; peopleNum <= DataHeatBalance::TotPeople; ++peopleNum) {
		auto& peopleHB = DataHeatBalance::People(peopleNum);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 1, peopleNum);
		sqliteBindText(m_nominalPeopleInsertStmt, 2, peopleHB.Name);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 3, peopleHB.ZonePtr);
		sqliteBindDouble(m_nominalPeopleInsertStmt, 4, peopleHB.NumberOfPeople);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 5, peopleHB.NumberOfPeoplePtr);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 6, peopleHB.ActivityLevelPtr);
		sqliteBindDouble(m_nominalPeopleInsertStmt, 7, peopleHB.FractionRadiant);
		sqliteBindDouble(m_nominalPeopleInsertStmt, 8, peopleHB.FractionConvected);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 9, peopleHB.WorkEffPtr);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 10, peopleHB.ClothingPtr);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 11, peopleHB.AirVelocityPtr);
		sqliteBindLogical(m_nominalPeopleInsertStmt, 12, peopleHB.Fanger);
		sqliteBindLogical(m_nominalPeopleInsertStmt, 13, peopleHB.Pierce);
		sqliteBindLogical(m_nominalPeopleInsertStmt, 14, peopleHB.KSU);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 15, peopleHB.MRTCalcType);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 16, peopleHB.SurfacePtr);
		sqliteBindText(m_nominalPeopleInsertStmt, 17, peopleHB.AngleFactorListName);
		sqliteBindInteger(m_nominalPeopleInsertStmt, 18, peopleHB.AngleFactorListPtr);
		sqliteBindDouble(m_nominalPeopleInsertStmt, 19, peopleHB.UserSpecSensFrac);
		sqliteBindLogical(m_nominalPeopleInsertStmt, 20, peopleHB.Show55Warning);

		sqliteStepCommand(m_nominalPeopleInsertStmt);
		sqliteResetCommand(m_nominalPeopleInsertStmt);
	}
}

void SQLite::createSQLiteNominalElectricEquipmentTable()
{
	for(int elecEquipNum = 1; elecEquipNum <= DataHeatBalance::TotElecEquip; ++elecEquipNum) {
		auto& elecEquipHB = DataHeatBalance::ZoneElectric(elecEquipNum);
		sqliteBindInteger(m_nominalElectricEquipmentInsertStmt, 1, elecEquipNum);
		sqliteBindText(m_nominalElectricEquipmentInsertStmt, 2, elecEquipHB.Name);
		sqliteBindInteger(m_nominalElectricEquipmentInsertStmt, 3, elecEquipHB.ZonePtr);
		sqliteBindInteger(m_nominalElectricEquipmentInsertStmt, 4, elecEquipHB.SchedPtr);
		sqliteBindDouble(m_nominalElectricEquipmentInsertStmt, 5, elecEquipHB.DesignLevel);
		sqliteBindDouble(m_nominalElectricEquipmentInsertStmt, 6, elecEquipHB.FractionLatent);
		sqliteBindDouble(m_nominalElectricEquipmentInsertStmt, 7, elecEquipHB.FractionRadiant);
		sqliteBindDouble(m_nominalElectricEquipmentInsertStmt, 8, elecEquipHB.FractionLost);
		sqliteBindDouble(m_nominalElectricEquipmentInsertStmt, 9, elecEquipHB.FractionConvected);
		sqliteBindText(m_nominalElectricEquipmentInsertStmt, 10, elecEquipHB.EndUseSubcategory);

		sqliteStepCommand(m_nominalElectricEquipmentInsertStmt);
		sqliteResetCommand(m_nominalElectricEquipmentInsertStmt);
	}
}

void SQLite::createSQLiteNominalGasEquipmentTable()
{
	for(int gasEquipNum = 1; gasEquipNum <= DataHeatBalance::TotGasEquip; ++gasEquipNum) {
		auto& gasEquipHB = DataHeatBalance::ZoneGas(gasEquipNum);
		sqliteBindInteger(m_nominalGasEquipmentInsertStmt, 1, gasEquipNum);
		sqliteBindText(m_nominalGasEquipmentInsertStmt, 2, gasEquipHB.Name);
		sqliteBindInteger(m_nominalGasEquipmentInsertStmt, 3, gasEquipHB.ZonePtr);
		sqliteBindInteger(m_nominalGasEquipmentInsertStmt, 4, gasEquipHB.SchedPtr);
		sqliteBindDouble(m_nominalGasEquipmentInsertStmt, 5, gasEquipHB.DesignLevel);
		sqliteBindDouble(m_nominalGasEquipmentInsertStmt, 6, gasEquipHB.FractionLatent);
		sqliteBindDouble(m_nominalGasEquipmentInsertStmt, 7, gasEquipHB.FractionRadiant);
		sqliteBindDouble(m_nominalGasEquipmentInsertStmt, 8, gasEquipHB.FractionLost);
		sqliteBindDouble(m_nominalGasEquipmentInsertStmt, 9, gasEquipHB.FractionConvected);
		sqliteBindText(m_nominalGasEquipmentInsertStmt, 10, gasEquipHB.EndUseSubcategory);

		sqliteStepCommand(m_nominalGasEquipmentInsertStmt);
		sqliteResetCommand(m_nominalGasEquipmentInsertStmt);
	}
}

void SQLite::createSQLiteNominalSteamEquipmentTable()
{
	for(int steamEquipNum = 1; steamEquipNum <= DataHeatBalance::TotStmEquip; ++steamEquipNum) {
		auto& steamEquipHB = DataHeatBalance::ZoneSteamEq(steamEquipNum);
		sqliteBindInteger(m_nominalSteamEquipmentInsertStmt, 1, steamEquipNum);
		sqliteBindText(m_nominalSteamEquipmentInsertStmt, 2, steamEquipHB.Name);
		sqliteBindInteger(m_nominalSteamEquipmentInsertStmt, 3, steamEquipHB.ZonePtr);
		sqliteBindInteger(m_nominalSteamEquipmentInsertStmt, 4, steamEquipHB.SchedPtr);
		sqliteBindDouble(m_nominalSteamEquipmentInsertStmt, 5, steamEquipHB.DesignLevel);
		sqliteBindDouble(m_nominalSteamEquipmentInsertStmt, 6, steamEquipHB.FractionLatent);
		sqliteBindDouble(m_nominalSteamEquipmentInsertStmt, 7, steamEquipHB.FractionRadiant);
		sqliteBindDouble(m_nominalSteamEquipmentInsertStmt, 8, steamEquipHB.FractionLost);
		sqliteBindDouble(m_nominalSteamEquipmentInsertStmt, 9, steamEquipHB.FractionConvected);
		sqliteBindText(m_nominalSteamEquipmentInsertStmt, 10, steamEquipHB.EndUseSubcategory);

		sqliteStepCommand(m_nominalSteamEquipmentInsertStmt);
		sqliteResetCommand(m_nominalSteamEquipmentInsertStmt);
	}
}

void SQLite::createSQLiteNominalHotWaterEquipmentTable()
{
	for(int hWEquipNum = 1; hWEquipNum <= DataHeatBalance::TotHWEquip; ++hWEquipNum) {
		auto& zoneHW = DataHeatBalance::ZoneHWEq(hWEquipNum);
		sqliteBindInteger(m_nominalHotWaterEquipmentInsertStmt, 1, hWEquipNum);
		sqliteBindText(m_nominalHotWaterEquipmentInsertStmt, 2, zoneHW.Name);
		sqliteBindInteger(m_nominalHotWaterEquipmentInsertStmt, 3, zoneHW.ZonePtr);
		sqliteBindInteger(m_nominalHotWaterEquipmentInsertStmt, 4, zoneHW.SchedPtr);
		sqliteBindDouble(m_nominalHotWaterEquipmentInsertStmt, 5, zoneHW.DesignLevel);
		sqliteBindDouble(m_nominalHotWaterEquipmentInsertStmt, 6, zoneHW.FractionLatent);
		sqliteBindDouble(m_nominalHotWaterEquipmentInsertStmt, 7, zoneHW.FractionRadiant);
		sqliteBindDouble(m_nominalHotWaterEquipmentInsertStmt, 8, zoneHW.FractionLost);
		sqliteBindDouble(m_nominalHotWaterEquipmentInsertStmt, 9, zoneHW.FractionConvected);
		sqliteBindText(m_nominalHotWaterEquipmentInsertStmt, 10, zoneHW.EndUseSubcategory);

		sqliteStepCommand(m_nominalHotWaterEquipmentInsertStmt);
		sqliteResetCommand(m_nominalHotWaterEquipmentInsertStmt);
	}
}

void SQLite::createSQLiteNominalOtherEquipmentTable()
{
	for(int otherEquipNum = 1; otherEquipNum <= DataHeatBalance::TotOthEquip; ++otherEquipNum) {
		auto& otherEquip = DataHeatBalance::ZoneOtherEq(otherEquipNum);
		sqliteBindInteger(m_nominalOtherEquipmentInsertStmt, 1, otherEquipNum);
		sqliteBindText(m_nominalOtherEquipmentInsertStmt, 2, otherEquip.Name);
		sqliteBindInteger(m_nominalOtherEquipmentInsertStmt, 3, otherEquip.ZonePtr);
		sqliteBindInteger(m_nominalOtherEquipmentInsertStmt, 4, otherEquip.SchedPtr);
		sqliteBindDouble(m_nominalOtherEquipmentInsertStmt, 5, otherEquip.DesignLevel);
		sqliteBindDouble(m_nominalOtherEquipmentInsertStmt, 6, otherEquip.FractionLatent);
		sqliteBindDouble(m_nominalOtherEquipmentInsertStmt, 7, otherEquip.FractionRadiant);
		sqliteBindDouble(m_nominalOtherEquipmentInsertStmt, 8, otherEquip.FractionLost);
		sqliteBindDouble(m_nominalOtherEquipmentInsertStmt, 9, otherEquip.FractionConvected);
		sqliteBindText(m_nominalOtherEquipmentInsertStmt, 10, otherEquip.EndUseSubcategory);

		sqliteStepCommand(m_nominalOtherEquipmentInsertStmt);
		sqliteResetCommand(m_nominalOtherEquipmentInsertStmt);
	}
}

void SQLite::createSQLiteNominalBaseboardHeatTable()
{
	for(int bBHeatNum = 1; bBHeatNum <= DataHeatBalance::TotBBHeat; ++bBHeatNum) {
		auto& baseboardHeat = DataHeatBalance::ZoneBBHeat(bBHeatNum);
		sqliteBindInteger(m_nominalBaseboardHeatInsertStmt, 1, bBHeatNum);
		sqliteBindText(m_nominalBaseboardHeatInsertStmt, 2, baseboardHeat.Name);
		sqliteBindInteger(m_nominalBaseboardHeatInsertStmt, 3, baseboardHeat.ZonePtr);
		sqliteBindInteger(m_nominalBaseboardHeatInsertStmt, 4, baseboardHeat.SchedPtr);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 5, baseboardHeat.CapatLowTemperature);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 6, baseboardHeat.LowTemperature);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 7, baseboardHeat.CapatHighTemperature);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 8, baseboardHeat.HighTemperature);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 9, baseboardHeat.FractionRadiant);
		sqliteBindDouble(m_nominalBaseboardHeatInsertStmt, 10, baseboardHeat.FractionConvected);
		sqliteBindText(m_nominalBaseboardHeatInsertStmt, 11, baseboardHeat.EndUseSubcategory);

		sqliteStepCommand(m_nominalBaseboardHeatInsertStmt);
		sqliteResetCommand(m_nominalBaseboardHeatInsertStmt);
	}
}

void SQLite::createSQLiteInfiltrationTable()
{
	for(int stmtNum = 1; stmtNum <= DataHeatBalance::TotInfiltration; ++stmtNum) {
		auto& infiltration = DataHeatBalance::Infiltration(stmtNum);
		sqliteBindInteger(m_infiltrationInsertStmt, 1, stmtNum);
		sqliteBindText(m_infiltrationInsertStmt, 2, infiltration.Name);
		sqliteBindInteger(m_infiltrationInsertStmt, 3, infiltration.ZonePtr);
		sqliteBindInteger(m_infiltrationInsertStmt, 4, infiltration.SchedPtr);
		sqliteBindDouble(m_infiltrationInsertStmt, 5, infiltration.DesignLevel);

		sqliteStepCommand(m_infiltrationInsertStmt);
		sqliteResetCommand(m_infiltrationInsertStmt);
	}
}

void SQLite::createSQLiteVentilationTable()
{
	for(int stmtNum = 1; stmtNum <= DataHeatBalance::TotVentilation; ++stmtNum) {
		auto& ventilation = DataHeatBalance::Ventilation(stmtNum);
		sqliteBindInteger(m_ventilationInsertStmt, 1, stmtNum);
		sqliteBindText(m_ventilationInsertStmt, 2, ventilation.Name);
		sqliteBindInteger(m_ventilationInsertStmt, 3, ventilation.ZonePtr);
		sqliteBindInteger(m_ventilationInsertStmt, 4, ventilation.SchedPtr);
		sqliteBindDouble(m_ventilationInsertStmt, 5, ventilation.DesignLevel);

		sqliteStepCommand(m_ventilationInsertStmt);
		sqliteResetCommand(m_ventilationInsertStmt);
	}
}

void SQLite::createSQLiteSurfacesTable()
{
	for(int surfaceNumber = 1; surfaceNumber <= DataSurfaces::TotSurfaces; ++surfaceNumber) {
		auto& surface = DataSurfaces::Surface(surfaceNumber);
		sqliteBindInteger(m_surfaceInsertStmt, 1, surfaceNumber);
		sqliteBindText(m_surfaceInsertStmt, 2, surface.Name);
		sqliteBindInteger(m_surfaceInsertStmt, 3, surface.Construction);
		sqliteBindText(m_surfaceInsertStmt, 4, DataSurfaces::cSurfaceClass(surface.Class));
		sqliteBindDouble(m_surfaceInsertStmt, 5, surface.Area);
		sqliteBindDouble(m_surfaceInsertStmt, 6, surface.GrossArea);
		sqliteBindDouble(m_surfaceInsertStmt, 7, surface.Perimeter);
		sqliteBindDouble(m_surfaceInsertStmt, 8, surface.Azimuth);
		sqliteBindDouble(m_surfaceInsertStmt, 9, surface.Height);
		sqliteBindDouble(m_surfaceInsertStmt, 10, surface.Reveal);
		sqliteBindInteger(m_surfaceInsertStmt, 11, surface.Shape);
		sqliteBindInteger(m_surfaceInsertStmt, 12, surface.Sides);
		sqliteBindDouble(m_surfaceInsertStmt, 13, surface.Tilt);
		sqliteBindDouble(m_surfaceInsertStmt, 14, surface.Width);
		sqliteBindLogical(m_surfaceInsertStmt, 15, surface.HeatTransSurf);
		sqliteBindInteger(m_surfaceInsertStmt, 16, surface.BaseSurf);
		sqliteBindInteger(m_surfaceInsertStmt, 17, surface.Zone);
		sqliteBindInteger(m_surfaceInsertStmt, 18, surface.ExtBoundCond);
		sqliteBindLogical(m_surfaceInsertStmt, 19, surface.ExtSolar);
		sqliteBindLogical(m_surfaceInsertStmt, 20, surface.ExtWind);

		sqliteStepCommand(m_surfaceInsertStmt);
		sqliteResetCommand(m_surfaceInsertStmt);
	}
}

void SQLite::createSQLiteConstructionsTable()
{
	for(int constructNum = 1; constructNum <= DataHeatBalance::TotConstructs; ++constructNum) {
		auto& construction = DataHeatBalance::Construct(constructNum);
		sqliteBindInteger(m_constructionInsertStmt, 1, constructNum);
		sqliteBindText(m_constructionInsertStmt, 2, construction.Name);
		sqliteBindInteger(m_constructionInsertStmt, 3, construction.TotLayers);
		sqliteBindInteger(m_constructionInsertStmt, 4, construction.TotSolidLayers);
		sqliteBindInteger(m_constructionInsertStmt, 5, construction.TotGlassLayers);

		for(int layerNum = 1; layerNum <= construction.TotLayers; ++layerNum) {
			sqliteBindInteger(m_constructionLayerInsertStmt, 1, constructNum);
			sqliteBindInteger(m_constructionLayerInsertStmt, 2, layerNum);
			sqliteBindInteger(m_constructionLayerInsertStmt, 3, construction.LayerPoint(layerNum));

			sqliteStepCommand(m_constructionLayerInsertStmt);
			sqliteResetCommand(m_constructionLayerInsertStmt);
		}

		sqliteBindDouble(m_constructionInsertStmt, 6, construction.InsideAbsorpVis);
		sqliteBindDouble(m_constructionInsertStmt, 7, construction.OutsideAbsorpVis);
		sqliteBindDouble(m_constructionInsertStmt, 8, construction.InsideAbsorpSolar);
		sqliteBindDouble(m_constructionInsertStmt, 9, construction.OutsideAbsorpSolar);
		sqliteBindDouble(m_constructionInsertStmt, 10, construction.InsideAbsorpThermal);
		sqliteBindDouble(m_constructionInsertStmt, 11, construction.OutsideAbsorpThermal);
		sqliteBindInteger(m_constructionInsertStmt, 12, construction.OutsideRoughness);
		sqliteBindLogical(m_constructionInsertStmt, 13, construction.TypeIsWindow);

		if(construction.TotGlassLayers == 0) {
			sqliteBindDouble(m_constructionInsertStmt, 14, construction.UValue);
		} else {
			sqliteBindDouble(m_constructionInsertStmt, 14, DataHeatBalance::NominalU(constructNum));
		}

		sqliteStepCommand(m_constructionInsertStmt);
		sqliteResetCommand(m_constructionInsertStmt);
	}
}

void SQLite::createSQLiteMaterialsTable()
{
	for(int materialNum = 1; materialNum <= DataHeatBalance::TotMaterials; ++materialNum) {
		auto& material = DataHeatBalance::Material(materialNum);
		sqliteBindInteger(m_materialInsertStmt, 1, materialNum);
		sqliteBindText(m_materialInsertStmt, 2, material.Name);
		sqliteBindInteger(m_materialInsertStmt, 3, material.Group);
		sqliteBindInteger(m_materialInsertStmt, 4, material.Roughness);
		sqliteBindDouble(m_materialInsertStmt, 5, material.Conductivity);
		sqliteBindDouble(m_materialInsertStmt, 6, material.Density);
		sqliteBindDouble(m_materialInsertStmt, 7, material.IsoMoistCap);
		sqliteBindDouble(m_materialInsertStmt, 8, material.Porosity);
		sqliteBindDouble(m_materialInsertStmt, 9, material.Resistance);
		sqliteBindLogical(m_materialInsertStmt, 10, material.ROnly);
		sqliteBindDouble(m_materialInsertStmt, 11, material.SpecHeat);
		sqliteBindDouble(m_materialInsertStmt, 12, material.ThermGradCoef);
		sqliteBindDouble(m_materialInsertStmt, 13, material.Thickness);
		sqliteBindDouble(m_materialInsertStmt, 14, material.VaporDiffus);

		sqliteStepCommand(m_materialInsertStmt);
		sqliteResetCommand(m_materialInsertStmt);
	}
}

void SQLite::createSQLiteZoneListTable()
{
	for(int listNum = 1; listNum <= DataHeatBalance::NumOfZoneLists; ++listNum) {
		auto& zoneList = DataHeatBalance::ZoneList(listNum);
		for(int zoneNum = 1; zoneNum <= zoneList.NumOfZones; ++zoneNum) {
			sqliteBindInteger(m_zoneListInsertStmt, 1, listNum);
			sqliteBindText(m_zoneListInsertStmt, 2, zoneList.Name);
			sqliteBindInteger(m_zoneListInsertStmt, 3, zoneList.Zone(zoneNum));

			sqliteStepCommand(m_zoneListInsertStmt);
			sqliteResetCommand(m_zoneListInsertStmt);
		}
	}
}

void SQLite::createSQLiteZoneGroupTable()
{
	for(int groupNum = 1; groupNum <= DataHeatBalance::NumOfZoneGroups; ++groupNum) {
		auto& zoneGroup = DataHeatBalance::ZoneGroup(groupNum);
		sqliteBindInteger(m_zoneGroupInsertStmt, 1, groupNum);
		sqliteBindText(m_zoneGroupInsertStmt, 2, zoneGroup.Name);
		sqliteBindInteger(m_zoneGroupInsertStmt, 3, zoneGroup.ZoneList);

		sqliteStepCommand(m_zoneGroupInsertStmt);
		sqliteResetCommand(m_zoneGroupInsertStmt);
	}
}

void SQLite::createSQLiteSchedulesTable()
{
	int numberOfSchedules = ScheduleManager::GetNumberOfSchedules();
	for(int scheduleNumber = 1; scheduleNumber <= numberOfSchedules; ++scheduleNumber) {
		sqliteBindInteger(m_scheduleInsertStmt, 1, scheduleNumber);
		sqliteBindText(m_scheduleInsertStmt, 2, ScheduleManager::GetScheduleName(scheduleNumber));
		sqliteBindText(m_scheduleInsertStmt, 3, ScheduleManager::GetScheduleType(scheduleNumber));
		sqliteBindDouble(m_scheduleInsertStmt, 4, ScheduleManager::GetScheduleMinValue(scheduleNumber));
		sqliteBindDouble(m_scheduleInsertStmt, 5, ScheduleManager::GetScheduleMaxValue(scheduleNumber));

		sqliteStepCommand(m_scheduleInsertStmt);
		sqliteResetCommand(m_scheduleInsertStmt);
	}
}

void SQLite::createZoneExtendedOutput()
{
	if( m_writeOutputToSQLite ) {
		createSQLiteZoneTable();
		createSQLiteNominalLightingTable();
		createSQLiteNominalPeopleTable();
		createSQLiteNominalElectricEquipmentTable();
		createSQLiteNominalGasEquipmentTable();
		createSQLiteNominalSteamEquipmentTable();
		createSQLiteNominalHotWaterEquipmentTable();
		createSQLiteNominalOtherEquipmentTable();
		createSQLiteNominalBaseboardHeatTable();
		createSQLiteInfiltrationTable();
		createSQLiteVentilationTable();
		createSQLiteSurfacesTable();
		createSQLiteConstructionsTable();
		createSQLiteMaterialsTable();
		createSQLiteZoneListTable();
		createSQLiteZoneGroupTable();
		createSQLiteRoomAirModelTable();
		createSQLiteSchedulesTable();
	}
}

void SQLite::createSQLiteEnvironmentPeriodRecord()
{
	if( m_writeOutputToSQLite ) {
		sqliteBindInteger(m_environmentPeriodInsertStmt, 1, DataEnvironment::CurEnvirNum);
		sqliteBindInteger(m_environmentPeriodInsertStmt, 2, 1);
		sqliteBindText(m_environmentPeriodInsertStmt, 3, DataEnvironment::EnvironmentName);
		sqliteBindInteger(m_environmentPeriodInsertStmt, 4, DataGlobals::KindOfSim);

		sqliteStepCommand(m_environmentPeriodInsertStmt);
		sqliteResetCommand(m_environmentPeriodInsertStmt);
	}
}

namespace SQLiteProcedures {

//// Data
//bool WriteOutputToSQLite( false );
//bool WriteTabularDataToSQLite( false );

} // SQLiteProcedures

} // EnergyPlus
