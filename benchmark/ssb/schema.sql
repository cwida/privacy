CREATE TABLE customer(c_custkey BIGINT, c_name VARCHAR, c_address VARCHAR, c_city VARCHAR,
 c_nation VARCHAR, c_region VARCHAR, c_phone VARCHAR, c_mktsegment VARCHAR);
CREATE TABLE supplier(s_suppkey BIGINT, s_name VARCHAR, s_address VARCHAR, s_city VARCHAR,
 s_nation VARCHAR, s_region VARCHAR, s_phone VARCHAR);
CREATE TABLE part(p_partkey BIGINT, p_name VARCHAR, p_mfgr VARCHAR, p_category VARCHAR,
 p_brand1 VARCHAR, p_color VARCHAR, p_type VARCHAR, p_size INTEGER, p_container VARCHAR);
CREATE TABLE date(d_datekey INTEGER, d_date VARCHAR, d_dayofweek VARCHAR, d_month VARCHAR,
 d_year INTEGER, d_yearmonthnum INTEGER, d_yearmonth VARCHAR, d_daynuminweek INTEGER,
 d_daynuminmonth INTEGER, d_daynuminyear INTEGER, d_monthnuminyear INTEGER,
 d_weeknuminyear INTEGER, d_sellingseason VARCHAR, d_lastdayinweekfl INTEGER,
 d_lastdayinmonthfl INTEGER, d_holidayfl INTEGER, d_weekdayfl INTEGER);
CREATE TABLE lineorder(lo_orderkey BIGINT, lo_linenumber INTEGER, lo_custkey BIGINT,
 lo_partkey BIGINT, lo_suppkey BIGINT, lo_orderdate INTEGER, lo_orderpriority VARCHAR,
 lo_shippriority INTEGER, lo_quantity BIGINT, lo_extendedprice BIGINT, lo_ordtotalprice BIGINT,
 lo_discount BIGINT, lo_revenue BIGINT, lo_supplycost BIGINT, lo_tax BIGINT,
 lo_commitdate INTEGER, lo_shipmode VARCHAR);
