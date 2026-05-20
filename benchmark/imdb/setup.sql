ALTER TABLE name ADD PRIVACY_KEY (id);
ALTER TABLE name SET PU;
ALTER PU TABLE name ADD PROTECTED (name, gender);

ALTER TABLE cast_info ADD PRIVACY_LINK (person_id) REFERENCES name (id);
ALTER TABLE aka_name ADD PRIVACY_LINK (person_id) REFERENCES name (id);
ALTER TABLE person_info ADD PRIVACY_LINK (person_id) REFERENCES name (id);
