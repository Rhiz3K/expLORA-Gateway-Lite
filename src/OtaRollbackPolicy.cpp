// Do not include Arduino.h here: its declaration of verifyRollbackLater()
// carries the framework's weak attribute, which would also make this override
// weak. This strong definition keeps a new OTA image pending until setup()
// explicitly confirms it after critical initialization.
extern "C" bool verifyRollbackLater()
{
    return true;
}
