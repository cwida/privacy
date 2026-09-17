-- Same age domains and positive-expenditure subset as AHRQ exercise_1a.
SELECT person_id, mask_lo, mask_hi,
       'mean_spenders_age_' || CASE WHEN age < 65 THEN 'under65' ELSE '65plus' END AS metric,
       weight * expenditure AS numerator, weight AS denominator, true AS is_ratio
FROM persons
WHERE expenditure > 0
