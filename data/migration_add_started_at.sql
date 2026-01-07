-- Migration: Add started_at column to rooms table
-- This migration adds server-side timer support

ALTER TABLE rooms ADD COLUMN started_at INTEGER;

-- Update existing IN_PROGRESS rooms to have a started_at value
-- Set to created_at for rooms that are already in progress
UPDATE rooms
SET started_at = created_at
WHERE status = 'IN_PROGRESS' AND started_at IS NULL;
