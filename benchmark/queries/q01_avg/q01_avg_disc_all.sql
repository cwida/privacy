SELECT
    avg(l_discount) AS avg_disc
FROM
    lineitem
WHERE
    l_shipdate <= CAST('1998-09-02' AS date);
