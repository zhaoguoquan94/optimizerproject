/*-------------------------------------------------------------------------
 *
 * tuple_transformer.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /n-store/src/bridge/tuple_transformer.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <iostream>

#include "backend/bridge/dml/tuple/tuple_transformer.h"
#include "backend/common/logger.h"
#include "backend/common/value_peeker.h"
#include "backend/storage/tuple.h"
#include "backend/common/types.h"
#include "backend/bridge/ddl/ddl.h"

#include "access/htup_details.h"
#include "nodes/print.h"

namespace peloton {
namespace bridge {

/**
 * @brief Convert from Datum to Value.
 * @return converted Value.
 */
Value TupleTransformer::GetValue(Datum datum, Oid atttypid) {
  Value value;

  switch (atttypid) {
    case POSTGRES_VALUE_TYPE_SMALLINT:
    {
      int16_t smallint = DatumGetInt16(datum);
      LOG_TRACE("%d\n", smallint);
      value = ValueFactory::GetSmallIntValue(smallint);
    }
    break;

    case POSTGRES_VALUE_TYPE_INTEGER:
    {
      int32_t integer = DatumGetInt32(datum);
      LOG_TRACE("%d\n", integer);
      value = ValueFactory::GetIntegerValue(integer);
    }
    break;

    case POSTGRES_VALUE_TYPE_BIGINT:
    {
      int64_t bigint = DatumGetInt64(datum);
      LOG_TRACE("%ld\n", bigint);
      value = ValueFactory::GetBigIntValue(bigint);
    }
    break;

    /*
     * In PG, BPCHAR and VARCHAR and TEXT are represented using
     * 'struct varlena',
     * which is a 4-byte header followed by the meat.
     * However, the 4-byte header should not be accessed directly.
     * It should be used in MACROS:
     * VARSIZE(ptr), VARDATA(ptr) and VARHDRSZ.
     * NB1: VARSIZE(ptr) is the size of the meat PLUS the header.
     * NB2: DON'T assume strings have terminating-null's.
     */
    case POSTGRES_VALUE_TYPE_BPCHAR:
    {
      struct varlena* bpcharptr = reinterpret_cast<struct varlena*>(datum);
      int len = VARSIZE(bpcharptr) - VARHDRSZ;
      char* varchar = static_cast<char *>(VARDATA(bpcharptr));
      Pool* data_pool = nullptr;
      std::string str(varchar, len);
      LOG_TRACE("VARSIZE = %d , bpchar = \"%s\"", len, str.c_str());
      value = ValueFactory::GetStringValue(str, data_pool);

    }
    break;

    case POSTGRES_VALUE_TYPE_VARCHAR2:
    {
      struct varlena* varlenptr = reinterpret_cast<struct varlena*>(datum);
      int len = VARSIZE(varlenptr) - VARHDRSZ;
      char* varchar = static_cast<char *>(VARDATA(varlenptr));
      Pool* data_pool = nullptr;
      std::string str(varchar, len);
      LOG_TRACE("VARSIZE = %d , varchar = \"%s\"", len, str.c_str());
      value = ValueFactory::GetStringValue(str, data_pool);
    }
    break;

    case POSTGRES_VALUE_TYPE_TEXT:
    {
      struct varlena* textptr = reinterpret_cast<struct varlena*>(datum);
      int len = VARSIZE(textptr) - VARHDRSZ;
      char* varchar = static_cast<char *>(VARDATA(textptr));
      Pool* data_pool = nullptr;
      std::string str(varchar, len);
      LOG_TRACE("VARSIZE = %d , text = \"%s\"", len, str.c_str());
      value = ValueFactory::GetStringValue(str, data_pool);
    }
    break;

    case POSTGRES_VALUE_TYPE_TIMESTAMPS:
    {
      long int timestamp = DatumGetInt64(datum);
      value = ValueFactory::GetTimestampValue(timestamp);
    }
    break;

    default:
      LOG_ERROR("Unknown atttypeid : %u ", atttypid);
      break;
  }

  return value;
}

/**
 * @brief Convert from Value to Datum.
 * @return converted Datum.
 */
Datum TupleTransformer::GetDatum(Value value) {
  ValueType value_type;
  Datum datum;

  value_type = value.GetValueType();
  switch (value_type) {

    case VALUE_TYPE_SMALLINT:
    {
      int16_t smallint = ValuePeeker::PeekSmallInt(value);
      LOG_TRACE("%d\n", smallint);
      datum = Int16GetDatum(smallint);
    }
    break;

    case VALUE_TYPE_INTEGER:
    {
      int32_t integer = ValuePeeker::PeekInteger(value);
      LOG_TRACE("%d\n", integer);
      datum = Int32GetDatum(integer);
    }
    break;

    case VALUE_TYPE_BIGINT:
    {
      int64_t bigint = ValuePeeker::PeekBigInt(value);
      LOG_TRACE("%ld\n", bigint);
      datum = Int64GetDatum(bigint);
    }
    break;

    case VALUE_TYPE_DOUBLE:
    {
      double double_precision = ValuePeeker::PeekDouble(value);
      LOG_TRACE("%f\n", double_precision);
      datum = Float8GetDatum(double_precision);
    }
    break;

    case VALUE_TYPE_VARCHAR:
    {
      // VARCHAR should be stored in a varlena in PG
      // We use palloc() to create a new varlena here.
      auto data_len = ValuePeeker::PeekObjectLength(value);
      auto data = ValuePeeker::PeekObjectValue(value);
      auto va_len = data_len + VARHDRSZ;

      if(va_len > 200) {
        LOG_INFO("VARLENA :: %d data_len : %d ", va_len, data_len);
      }

      struct varlena* vaptr = (struct varlena*)palloc(va_len);
      SET_VARSIZE(vaptr, va_len);
      ::memcpy(VARDATA(vaptr), data, data_len);

      LOG_TRACE("len = %d , str = \"%s\" \n", data_len, std::string((char*)data, data_len).c_str());

      datum = (Datum)vaptr;

//      char *variable_character = (char *) ValuePeeker::PeekObjectValue(value);
//      LOG_TRACE("%s\n", variable_character);
//      datum = CStringGetDatum(variable_character);
    }
    break;

    case VALUE_TYPE_TIMESTAMP:
    {
      long int timestamp = ValuePeeker::PeekTimestamp(value);
      datum = Int64GetDatum(timestamp);
      LOG_TRACE("%s\n",DatumGetCString(timestamp));
    }
    break;

    default:
      datum = PointerGetDatum(nullptr);
      LOG_TRACE("Unrecognized value type : %u\n", value_type);
      break;
  }

  return datum;
}

/**
 * @brief Convert a Postgres tuple into Peloton tuple
 * @param slot Postgres tuple
 * @param schema Peloton scheme of the table to which the tuple belongs
 * @return a Peloton tuple
 */
storage::Tuple *TupleTransformer::GetPelotonTuple(TupleTableSlot *slot,
                                                 const catalog::Schema *schema) {
  assert(slot);

  TupleDesc tuple_desc = slot->tts_tupleDescriptor;
  int natts = tuple_desc->natts;
  bool isnull;

  // Allocate space for a new tuple with given schema
  storage::Tuple *tuple = new storage::Tuple(schema, true);

  // Go over each attribute and convert Datum to Value
  for (oid_t att_itr = 0; att_itr < natts; ++att_itr) {
    Datum attr = slot_getattr(slot, att_itr + 1, &isnull);
    if (isnull)
      continue;

    Form_pg_attribute attribute_info = tuple_desc->attrs[att_itr];
    Oid attribute_type_id = attribute_info->atttypid;

    Value value = GetValue(attr, attribute_type_id);
    tuple->SetValue(att_itr++, value);
  }

  return tuple;
}

/**
 * @brief Convert a Peloton tuple into Postgres tuple slot
 * @param tuple Peloton tuple
 * @return a Postgres tuple
 */
TupleTableSlot *TupleTransformer::GetPostgresTuple(storage::Tuple *tuple,
                                                   TupleDesc tuple_desc) {
  assert(tuple);
  assert(tuple_desc);

  TupleTableSlot *slot = NULL;
  HeapTuple heap_tuple;
  int natts = tuple_desc->natts;
  Datum *datums;
  bool *nulls;

  if(tuple->GetColumnCount() != natts) {
    LOG_WARN("tuple attr count : %u tuple desc attr count : %d \n",
             tuple->GetColumnCount(), natts);
    return nullptr;
  }

  // Allocate space for datums
  datums = (Datum *) palloc(natts * sizeof(Datum));
  nulls = (bool *) palloc(natts * sizeof(bool));

  // Go over each attribute and convert Value to Datum
  for (oid_t att_itr = 0; att_itr < natts; ++att_itr) {
    Value value = tuple->GetValue(att_itr);

    // NB: this may allocate varlena
    Datum datum = GetDatum(value);

    datums[att_itr] = datum;
    nulls[att_itr] = tuple->IsNull(att_itr) ? true : false;
  }

  // Construct tuple
  /*
   * I believe PG does a deep copy in heap_form_tuple(),
   * which means datums[] should be freed in the current function.
   * - Qiang 7/21/15
   */
  heap_tuple = heap_form_tuple(tuple_desc, datums, nulls);

  // Construct slot
  slot = MakeSingleTupleTableSlot(tuple_desc);

  // Store tuple in slot
  /*
   * This function just sets a point in slot to
   * the heap_tuple.
   */
  ExecStoreTuple(heap_tuple, slot, InvalidBuffer, true);

  // Clean up (A-B): seems we have to do the cleaning manually (no PG utility?)
  // (A) Clean up any possible varlena's
  for (oid_t att_itr = 0; att_itr < natts; ++att_itr) {
    if(tuple_desc->attrs[att_itr]->attlen < 0){ // should be a varlen

      assert(tuple_desc->attrs[att_itr]->attbyval == false);
      // For now, only VARCHAR would be transformed to a varlena (see GetDatum() above)
      assert(tuple->GetValue(att_itr).GetValueType() == VALUE_TYPE_VARCHAR);

      pfree((void*)(datums[att_itr]));
    }
  }
  // (B) Free the datum array itself
  pfree(datums);

  pfree(nulls);

  return slot;
}


} // namespace bridge
} // namespace peloton
