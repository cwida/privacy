PRAGMA clear_privacy_metadata;
SET priv_check=false;
ALTER TABLE hits ADD PRIVACY_KEY (UserID);
ALTER TABLE hits SET PU;
ALTER PU TABLE hits ADD PROTECTED (ClientIP);
