-- AHRQ use_events_2016: OB/OP, all/physician events, five payment sources.
SELECT p.person_id, p.mask_lo, p.mask_hi,
       e.kind || '_' || d.domain || '_' || m.metric AS metric,
       p.weight * m.numerator AS numerator,
       CASE WHEN m.is_ratio THEN p.weight ELSE 0 END AS denominator, m.is_ratio
FROM persons p JOIN events e USING (person_id),
LATERAL (VALUES ('all', true), ('physician', e.physician = 1)) d(domain, keep),
LATERAL (VALUES ('event_count', 1.0, false), ('mean_self', e.self, true),
 ('mean_private', e.private, true), ('mean_medicare', e.medicare, true),
 ('mean_medicaid', e.medicaid, true), ('mean_other', e.other, true)
) m(metric, numerator, is_ratio)
WHERE d.keep
