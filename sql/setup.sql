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
-- Gaming Teams v1.1: admin panel (rename, join code, kick, ban, delete) and team goals.
-- Run once in the Supabase SQL Editor after setup.sql. Safe to run again.

-- ----- Admin columns. owner_key and join_code are hidden from everyone (only the functions below read them)
alter table public.team_projects add column if not exists owner_key text not null default '';
alter table public.team_projects add column if not exists join_code text not null default '';
alter table public.team_projects add column if not exists banned text[] not null default '{}';
alter table public.team_projects add column if not exists locked boolean generated always as (join_code <> '') stored;
alter table public.team_members add column if not exists kicked boolean not null default false;

-- Only safe columns can be read or changed directly
revoke all on public.team_projects from anon, authenticated;
grant select (code, host, level_name, snapshot, snapshot_event, saved_by, objects, created_at, updated_at, locked)
  on public.team_projects to anon, authenticated;
grant insert (code, host, level_name, snapshot, snapshot_event, saved_by, objects, owner_key)
  on public.team_projects to anon, authenticated;
grant update (snapshot, snapshot_event, saved_by, objects, updated_at)
  on public.team_projects to anon, authenticated;

revoke all on public.team_members from anon, authenticated;
grant select on public.team_members to anon, authenticated;
grant insert (room, client, name, cursor_x, cursor_y, has_cursor, last_seen) on public.team_members to anon, authenticated;
grant update (room, client, name, cursor_x, cursor_y, has_cursor, last_seen) on public.team_members to anon, authenticated;

-- Admin actions: only work with the level's admin key
create or replace function public.team_admin(p_code text, p_key text, p_action text, p_value text default '')
returns text language plpgsql security definer set search_path = public as $$
declare proj team_projects%rowtype;
begin
  select * into proj from team_projects where code = p_code;
  if not found then return 'missing'; end if;
  if proj.owner_key = '' or proj.owner_key is distinct from p_key then return 'denied'; end if;

  if p_action = 'info' then
    return json_build_object('code', proj.join_code, 'banned', proj.banned)::text;
  elsif p_action = 'rename' then
    if length(trim(p_value)) = 0 or length(p_value) > 40 then return 'bad name'; end if;
    update team_projects set level_name = trim(p_value) where code = p_code;
  elsif p_action = 'set_code' then
    if length(p_value) > 20 then return 'code too long'; end if;
    update team_projects set join_code = p_value where code = p_code;
  elsif p_action = 'kick' then
    update team_members set kicked = true where room = p_code and client = p_value;
  elsif p_action = 'ban' then
    if length(trim(p_value)) = 0 then return 'no name'; end if;
    update team_projects set banned = array_append(array_remove(banned, lower(trim(p_value))), lower(trim(p_value))) where code = p_code;
    update team_members set kicked = true where room = p_code and lower(name) = lower(trim(p_value));
  elsif p_action = 'unban' then
    update team_projects set banned = array_remove(banned, lower(trim(p_value))) where code = p_code;
  elsif p_action = 'delete' then
    delete from team_projects where code = p_code;
  else
    return 'unknown action';
  end if;
  return 'ok';
end;
$$;

-- Before opening a level: banned? needs a join code?
create or replace function public.team_check(p_code text, p_name text, p_join_code text default '')
returns text language plpgsql security definer set search_path = public as $$
declare proj team_projects%rowtype;
begin
  select * into proj from team_projects where code = p_code;
  if not found then return 'missing'; end if;
  if lower(trim(p_name)) = any(proj.banned) then return 'banned'; end if;
  if proj.join_code <> '' and proj.join_code is distinct from p_join_code then
    return case when coalesce(p_join_code, '') = '' then 'code' else 'wrong' end;
  end if;
  return 'ok';
end;
$$;

revoke all on function public.team_admin(text, text, text, text) from public;
revoke all on function public.team_check(text, text, text) from public;
grant execute on function public.team_admin(text, text, text, text) to anon, authenticated;
grant execute on function public.team_check(text, text, text) to anon, authenticated;

-- ----- Team goals: what everyone wants to do on a level
create table if not exists public.team_goals (
  id         bigint generated always as identity primary key,
  room       text not null references public.team_projects(code) on delete cascade,
  author     text not null default '',
  text       text not null check (length(text) between 1 and 120),
  done       boolean not null default false,
  done_by    text not null default '',
  created_at timestamptz not null default now()
);
create index if not exists team_goals_room on public.team_goals (room, id);

alter table public.team_goals enable row level security;
drop policy if exists "goals read"   on public.team_goals;
drop policy if exists "goals add"    on public.team_goals;
drop policy if exists "goals update" on public.team_goals;
create policy "goals read"   on public.team_goals for select using (true);
create policy "goals add"    on public.team_goals for insert with check (true);
create policy "goals update" on public.team_goals for update using (true) with check (true);

revoke all on public.team_goals from anon, authenticated;
grant select on public.team_goals to anon, authenticated;
grant insert (room, author, text) on public.team_goals to anon, authenticated;
grant update (done, done_by) on public.team_goals to anon, authenticated;

-- Removing a goal: whoever wrote it, or the level's admin
create or replace function public.team_goal_remove(p_id bigint, p_name text, p_key text default '')
returns text language plpgsql security definer set search_path = public as $$
declare g team_goals%rowtype; proj team_projects%rowtype;
begin
  select * into g from team_goals where id = p_id;
  if not found then return 'ok'; end if;
  select * into proj from team_projects where code = g.room;
  if lower(g.author) = lower(trim(p_name)) or (proj.owner_key <> '' and proj.owner_key = p_key) then
    delete from team_goals where id = p_id;
    return 'ok';
  end if;
  return 'denied';
end;
$$;
revoke all on function public.team_goal_remove(bigint, text, text) from public;
grant execute on function public.team_goal_remove(bigint, text, text) to anon, authenticated;
