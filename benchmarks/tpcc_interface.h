#ifndef _NDB_BENCH_TPCC_INTERFACE_H_
#define _NDB_BENCH_TPCC_INTERFACE_H_

#include "../record/encoder.h"
#include "../record/inline_str.h"
#include "../macros.h"

struct tpcc_new_order_args {
  uint warehouse_id;
  uint districtID;
  uint customerID;
  uint numItems;
  uint itemIDs[15];
  uint supplierWarehouseIDs[15];
  uint orderQuantities[15];
  bool allLocal;
};

struct tpcc_delivery_args {
  uint warehouse_id;
  uint o_carrier_id;
};

struct tpcc_payment_args {
  uint warehouse_id;
  uint districtID;
  uint customerDistrictID;
  uint customerWarehouseID;
  float paymentAmount;
};

struct tpcc_order_status_args {
  uint warehouse_id;
  uint districtID;
};

struct tpcc_stock_level_args {
  uint warehouse_id;
  uint threshold;
  uint districtID;
};

union tpcc_args_union {
  tpcc_new_order_args new_order;
  tpcc_delivery_args delivery;
  tpcc_payment_args payment;
  tpcc_order_status_args order_status;
  tpcc_stock_level_args stock_level;
};

struct tpcc_generator {
  uint warehouse_id_start;
  uint warehouse_id_end;
  uint numWarehouses;
  int remote_item_pct;
  bool disable_xpartition_txn;
  bool uniform_item_dist;


  // utils for generating random #s and strings

  static inline ALWAYS_INLINE int
  RandomNumber(util::fast_random &r, int min, int max)
  {
    return util::check_between_inclusive
      ((int) (r.next_uniform() * (max - min + 1) + min), min, max);
  }

  static inline ALWAYS_INLINE int
  NonUniformRandom(util::fast_random &r, int A, int C, int min, int max)
  {
    return (((RandomNumber(r, 0, A) | RandomNumber(r, min, max)) + C) % (max - min + 1)) + min;
  }

  static constexpr inline ALWAYS_INLINE size_t
  NumItems()
  {
    return 100000;
  }

  static constexpr inline ALWAYS_INLINE size_t
  NumDistrictsPerWarehouse()
  {
    return 10;
  }

  static constexpr inline ALWAYS_INLINE size_t
  NumCustomersPerDistrict()
  {
    return 3000;
  }

  static inline ALWAYS_INLINE int
  GetItemId(util::fast_random &r, bool uniform_item_dist)
  {
    return util::check_between_inclusive(
        uniform_item_dist ?
          RandomNumber(r, 1, NumItems()) :
          NonUniformRandom(r, 8191, 7911, 1, NumItems()),
        1, (int) NumItems());
  }

  inline ALWAYS_INLINE int
  GetItemId(util::fast_random &r)
  {
    return GetItemId(r, this->uniform_item_dist);
  }

  static inline ALWAYS_INLINE int
  GetCustomerId(util::fast_random &r)
  {
    return util::check_between_inclusive
      (NonUniformRandom(r, 1023, 259, 1, NumCustomersPerDistrict()),
       1, (int) NumCustomersPerDistrict());
  }

  // pick a number between [start, end)
  static inline ALWAYS_INLINE unsigned
  PickWarehouseId(util::fast_random &r, unsigned start, unsigned end)
  {
    INVARIANT(start < end);
    const unsigned diff = end - start;
    if (diff == 1)
      return start;
    return (r.next() % diff) + start;
  }

  inline void
  choose_new_order_args(tpcc_new_order_args& a, util::fast_random& r) {
    a.warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
    a.districtID = RandomNumber(r, 1, 10);
    a.customerID = GetCustomerId(r);
    a.numItems = RandomNumber(r, 5, 15);
    a.allLocal = true;
    for (uint i = 0; i < a.numItems; i++) {
      a.itemIDs[i] = GetItemId(r);
      if (likely(!remote_item_pct ||
                 numWarehouses == 1 ||
                 RandomNumber(r, 1, 100) > remote_item_pct)) {
        a.supplierWarehouseIDs[i] = a.warehouse_id;
      } else {
        do {
          a.supplierWarehouseIDs[i] = RandomNumber(r, 1, numWarehouses);
        } while (a.supplierWarehouseIDs[i] == a.warehouse_id);
        a.allLocal = false;
      }
      a.orderQuantities[i] = RandomNumber(r, 1, 10);
    }
  }

  inline void
  choose_delivery_args(tpcc_delivery_args& a, util::fast_random& r) {
    a.warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
    a.o_carrier_id = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  }

  inline void
  choose_payment_args(tpcc_payment_args& a, util::fast_random& r) {
    a.warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
    a.districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
    if (likely(disable_xpartition_txn ||
               numWarehouses == 1 ||
               RandomNumber(r, 1, 100) <= 85)) {
      a.customerDistrictID = a.districtID;
      a.customerWarehouseID = a.warehouse_id;
    } else {
      a.customerDistrictID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
      do {
        a.customerWarehouseID = RandomNumber(r, 1, numWarehouses);
      } while (a.customerWarehouseID == a.warehouse_id);
    }
    a.paymentAmount = (float) (RandomNumber(r, 100, 500000) / 100.0);
  }

  inline void
  choose_order_status_args(tpcc_order_status_args& a, util::fast_random& r) {
    a.warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
    a.districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  }

  inline void
  choose_stock_level_args(tpcc_stock_level_args& a, util::fast_random& r) {
    a.warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
    a.threshold = RandomNumber(r, 10, 20);
    a.districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  }
};

#endif
