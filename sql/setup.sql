-- Gaming Teams: team levels that the whole team builds together, saved online.
-- Run once in Supabase: Dashboard -> SQL Editor -> New query -> paste -> Run.
-- Safe to run again. Uses the same Supabase project as GD Gaming Collab (separate tables).

-- Projects: one per team level. `snapshot` is the saved level (all objects + colors) up to `snapshot_event`.
-- Projects are never deleted automatically: that's the saved progress.
create table if not exists public.team_projects (
  code           text primary key check (code ~ '^[A-Z0-9]{4,8}$'),
  host           text not null default '',          -- who made the project
  level_name     text not null default '' check (length(level_name) <= 40),
  snapshot       text not null default '[]',
  snapshot_event bigint not null default 0,
  saved_by       text not null default '',
  objects        integer not null default 0,
  created_at     timestamptz not null default now(),
  updated_at     timestamptz not null default now()
);

-- Every batch of edits someone made (objects added / changed / deleted, colors changed).
create table if not exists public.team_events (
  id         bigint generated always as identity primary key,
  room       text not null references public.team_projects(code) on delete cascade,
  client     text not null,
  author     text not null default '',
  data       text not null,
  created_at timestamptz not null default now()
);
create index if not exists team_events_room_id on public.team_events (room, id);

-- Who's working on a project right now, and where their cursor is.
create table if not exists public.team_members (
  room       text not null references public.team_projects(code) on delete cascade,
  client     text not null,
  name       text not null default '',
  cursor_x   real not null default 0,
  cursor_y   real not null default 0,
  has_cursor boolean not null default false,
  last_seen  timestamptz not null default now(),
  primary key (room, client)
);
create index if not exists team_members_seen on public.team_members (last_seen);

alter table public.team_projects enable row level security;
alter table public.team_events   enable row level security;
alter table public.team_members  enable row level security;

drop policy if exists "projects read"   on public.team_projects;
drop policy if exists "projects create" on public.team_projects;
drop policy if exists "projects update" on public.team_projects;
create policy "projects read"   on public.team_projects for select using (true);
create policy "projects create" on public.team_projects for insert with check (true);
create policy "projects update" on public.team_projects for update using (true) with check (true);

drop policy if exists "team events read" on public.team_events;
drop policy if exists "team events add"  on public.team_events;
create policy "team events read" on public.team_events for select using (true);
create policy "team events add"  on public.team_events for insert with check (length(data) < 5000000);

drop policy if exists "team members read" on public.team_members;
drop policy if exists "team members join" on public.team_members;
drop policy if exists "team members ping" on public.team_members;
create policy "team members read" on public.team_members for select using (true);
create policy "team members join" on public.team_members for insert with check (true);
create policy "team members ping" on public.team_members for update using (true) with check (true);

grant select, insert, update on public.team_projects, public.team_members to anon, authenticated;
grant select, insert on public.team_events to anon, authenticated;

-- "Last edited" time for the project list
create or replace function public.team_touch_project() returns trigger
language plpgsql security definer set search_path = public as $$
begin
  update team_projects set updated_at = now() where code = new.room;
  return new;
end;
$$;
drop trigger if exists team_touch_on_event on public.team_events;
create trigger team_touch_on_event after insert on public.team_events
  for each row execute function public.team_touch_project();
