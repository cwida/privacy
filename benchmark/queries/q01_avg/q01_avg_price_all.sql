SELECT
    avg(l_extendedprice) AS avg_price
FROM
    lineitem
WHERE
    l_shipdate <= CAST('1998-09-02' AS date);
