#include "include.h"

extern MemoryContext myMemoryContext;
extern StringInfoData response;

static Oid SPI_gettypeid_my(TupleDesc tupdesc, int fnumber) {
    if (fnumber > tupdesc->natts || !fnumber || fnumber <= FirstLowInvalidHeapAttributeNumber) E("SPI_ERROR_NOATTRIBUTE");
    return (fnumber > 0 ? TupleDescAttr(tupdesc, fnumber - 1) : SystemAttributeDefinition(fnumber))->atttypid;
}

static char *SPI_getvalue_my2(TupleTableSlot *slot, TupleDesc tupdesc, int fnumber) {
    Oid oid = SPI_gettypeid_my(tupdesc, fnumber);
    bool isnull;
    Oid foutoid;
    bool typisvarlena;
    Datum val = slot_getattr(slot, fnumber, &isnull);
    if (isnull) return NULL;
    getTypeOutputInfo(oid, &foutoid, &typisvarlena);
    return OidOutputFunctionCall(foutoid, val);
}

const char *SPI_gettype_my(Oid oid) {
    const char *result;
    HeapTuple typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oid));
    if (!HeapTupleIsValid(typeTuple)) E("SPI_ERROR_TYPUNKNOWN");
    result = NameStr(((Form_pg_type)GETSTRUCT(typeTuple))->typname);
    ReleaseSysCache(typeTuple);
    return result;
}

static const char *SPI_gettype_my2(TupleDesc tupdesc, int fnumber) {
    return SPI_gettype_my(SPI_gettypeid_my(tupdesc, fnumber));
}

static bool receiveSlot(TupleTableSlot *slot, DestReceiver *self) {
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(myMemoryContext);
    if (!response.data) initStringInfo(&response);
    if (!response.len && slot->tts_tupleDescriptor->natts > 1) {
        for (int col = 1; col <= slot->tts_tupleDescriptor->natts; col++) {
            if (col > 1) appendStringInfoString(&response, "\t");
            appendStringInfo(&response, "%s::%s", SPI_fname(slot->tts_tupleDescriptor, col), SPI_gettype_my2(slot->tts_tupleDescriptor, col));
        }
    }
    if (response.len) appendStringInfoString(&response, "\n");
    for (int col = 1; col <= slot->tts_tupleDescriptor->natts; col++) {
        const char *value = SPI_getvalue_my2(slot, slot->tts_tupleDescriptor, col);
        if (col > 1) appendStringInfoString(&response, "\t");
        appendStringInfoString(&response, value ? value : "(null)");
        if (value) pfree((void *)value);
    }
    MemoryContextSwitchTo(oldMemoryContext);
    return true;
}

static void rStartup(DestReceiver *self, int operation, TupleDesc typeinfo) { }

static void rShutdown(DestReceiver *self) { }

static void rDestroy(DestReceiver *self) { }

static const DestReceiver DestReceiverMy = {.receiveSlot = receiveSlot, .rStartup = rStartup, .rShutdown = rShutdown, .rDestroy = rDestroy, .mydest = DestDebug};

DestReceiver *CreateDestReceiverMy(CommandDest dest) {
    return dest == DestDebug ? unconstify(DestReceiver *, &DestReceiverMy) : CreateDestReceiver(dest);
}
