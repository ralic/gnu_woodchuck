#! /bin/bash

file=$1

constraint=$2
if test x"$constraint" != x
then
  and_constraint="and ($constraint)"
fi

echo \'"$file"\' \'"$constraint"\'

# Time difference from UCT in hours.
tdelta='-5'

home=$(sqlite3 "$file" 'select filename from files where ROWID=1;' \
        | sed 's#^\(/[^/]\+/[^/]\+/\).*$#\1#')
hl=$(echo -n "$home/" | wc -c)
echo "Home directory appears to be $home"

sqlite3 "$file" \
  '-- Use a large memory cache
   pragma cache_size = 1000000000;

   drop table if exists log_filtered;
   drop table if exists log_coalesced;
   drop table if exists log_scans;
   drop table if exists l;

   create index if not exists index_log_time on log (time);

   select "Total number of files: " || count (*) from files;
   select "Total number of accesses: " || count (*) from log;   
   select "Log spans " || days || " days, "
          || (hours - days * 24) || " hours"
     from (select (seconds / 60 / 60) hours,
                  (seconds / 60 / 60 / 24) days
  	     from (select (max(time) - min(time)) seconds
                     from log));
   select " ";

   -- Join log and file, filter out any deletes and apply the user filter.
   create table log_filtered as
     select * from log join files
       where log.uid = files.uid and size_plus_one > 0 '"$and_constraint"'
       order by time;

   create index if not exists index_log_filtered_time on log_filtered (time);
   create index if not exists index_log_filtered_filename
     on log_filtered (filename);

   select "Filtered number of files: " || count (*)
     from (select distinct uid from log_filtered);
   select "Filtered number of accesses: " || count (*) from log_filtered;
   select count (*) || " Files; average accesses: " || avg (accesses)
     from (select filename, count (*) as accesses
             from log_filtered group by filename);
   select accesses || " accesses: " || count (*) || " files"
     from (select filename, count (*) as accesses
             from log_filtered group by filename)
     group by accesses order by accesses desc;
   select " ";

   -- Remove accesses that occured within a small window of the previous
   -- access of the same file.
   create table log_coalesced as
    select * from log_filtered l_outer
      where uid not in (select uid from log_filtered l
                          where l_outer.time - 5 * 60 <= l.time
                                and l.time <= l_outer.time
                                and ROWID < l_outer.ROWID)
      order by time;

   select "Coalesced number of files: " || count (*)
     from (select distinct uid from log_coalesced);
   select "Coalesced number of accesses: " || count (*) from log_coalesced;
   select count (*) || " Files; average accesses: " || avg (accesses)
     from (select filename, count (*) as accesses
             from log_coalesced group by filename);
   select accesses || " accesses: " || count (*) || " files"
     from (select filename, count (*) as accesses
             from log_coalesced group by filename)
     group by accesses order by accesses desc;
   select " ";

   create index if not exists index_log_coalesced_time on log_coalesced (time);

   -- Detect files belonging to a scan.
   create table log_scans as
     select *, (previous_delta >= -1 and next_delta <= 1)
                or previous2_delta >= -1
                or next2_delta <= 1 as scan
     from (select *, (next - time) as next_delta,
               (next2 - time) as next2_delta,
               (previous - time) as previous_delta,
               (previous2 - time) as previous2_delta
             from (select a.*,
                     COALESCE (b.time, a.time + -99999) previous2,
                     COALESCE (c.time, a.time + -99999) previous,
                     COALESCE (d.time, a.time + 99999) next,
                     COALESCE (e.time, a.time + 99999) next2
                    from log_coalesced a
                      LEFT JOIN log_coalesced b on a.rowid = b.rowid + 2
                      LEFT JOIN log_coalesced c on a.rowid = c.rowid + 1
                      LEFT JOIN log_coalesced d on a.rowid = d.rowid - 1
                      LEFT JOIN log_coalesced e on a.rowid = e.rowid - 2))
     order by time;

   -- Make it peerrty.
   create table l as
    select (time + "'"$tdelta"'" * 60 * 60) as time,
      strftime ("%Y", time, "unixepoch", "'"$tdelta"' hours") as year,
      strftime ("%j", time, "unixepoch", "'"$tdelta"' hours") as doy,
      strftime ("%m", time, "unixepoch", "'"$tdelta"' hours") as month,
      strftime ("%d", time, "unixepoch", "'"$tdelta"' hours") as day,
      strftime ("%H", time, "unixepoch", "'"$tdelta"' hours") as hour,
      (select case strftime ("%w", time, "unixepoch", "'"$tdelta"' hours")
       when "0" then "Sun"
       when "1" then "Mon"
       when "2" then "Tue"
       when "3" then "Wed"
       when "4" then "Thu"
       when "5" then "Fri"
       when "6" then "Sat"
       end) as dow,
      size_plus_one - 1 as size,
      filename,
      uid,
      previous2_delta, previous_delta, next_delta, next2_delta,  scan
     from log_scans
     order by time;

   create index if not exists index_l_time on l (time);
   create index index_l_filename on l (filename);


   
   select "Total scan accesses: " || count (*) from l where scan;
   select "Total non-scan accesses: " || count (*) from l where not scan;
   select " ";

   select "Accesses for hour ("
     || (select count (*)
           from (select * from l group by year, doy, hour))
     || " hours with accesses):";
   select strftime ("%Y-%m-%d %H:00 ", time, "unixepoch") || dow || ": "
          || accesses || " accesses, "
          || (case
                when accesses_bytes < 10 * 1024
                  then accesses_bytes || " Bytes"
                when accesses_bytes < 10 * 1024 * 1024
                  then (accesses_bytes / 1024) || " KB"
                else (accesses_bytes / 1024 / 1024) || " MB" end)
          || "; first: " || firsts || " (" || (100 * firsts / accesses) || "%), "
          || (case
                when firsts_bytes < 10 * 1024
                  then firsts_bytes || " Bytes"
                when firsts_bytes < 10 * 1024 * 1024
                  then (firsts_bytes / 1024) || " KB"
                else (firsts_bytes / 1024 / 1024) || " MB" end)
          || " (" || (((1 + firsts_bytes) * 100) / (1+accesses_bytes)) || "%)"
          || "; subsequent: " || subsequents || " (" || (100 * subsequents / accesses) || "%), "
          || (case
                when subsequents_bytes < 10 * 1024
                  then subsequents_bytes || " Bytes"
                when subsequents_bytes < 10 * 1024 * 1024
                  then (subsequents_bytes / 1024) || " KB"
                else (subsequents_bytes / 1024 / 1024) || " MB" end)
          || " (" || (((1 + subsequents_bytes) * 100)
                       / (1+accesses_bytes)) || "%)"
     from (select *, count (*) accesses,
       sum (size) accesses_bytes,
       sum (uid not in (select uid from l where ROWID < louter.ROWID)) firsts,
       sum (case when uid not in (select uid from l where ROWID < louter.ROWID)
                   then size
                 else 0
            end) firsts_bytes,
       sum (uid in (select uid from l where ROWID < louter.ROWID)) subsequents,
       sum (case when uid in (select uid from l where ROWID < louter.ROWID)
                   then size
                 else 0
            end) subsequents_bytes
       from l as louter
       group by year, doy, hour
       order by time);

   select "Accesses for day ("
     || (select count (*)
           from (select * from l group by year, doy))
     || " days with accesses):";
   select strftime ("%Y-%m-%d ", time, "unixepoch") || dow || ": "
          || accesses || " accesses, "
          || (case
                when accesses_bytes < 10 * 1024
                  then accesses_bytes || " Bytes"
                when accesses_bytes < 10 * 1024 * 1024
                  then (accesses_bytes / 1024) || " KB"
                else (accesses_bytes / 1024 / 1024) || " MB" end)
          || "; first: " || firsts || ", "
          || (case
                when firsts_bytes < 10 * 1024
                  then firsts_bytes || " Bytes"
                when firsts_bytes < 10 * 1024 * 1024
                  then (firsts_bytes / 1024) || " KB"
                else (firsts_bytes / 1024 / 1024) || " MB" end)
          || " (" || (((1 + firsts_bytes) * 100) / (1+accesses_bytes)) || "%)"
          || "; subsequent: " || subsequents || ", "
          || (case
                when subsequents_bytes < 10 * 1024
                  then subsequents_bytes || " Bytes"
                when subsequents_bytes < 10 * 1024 * 1024
                  then (subsequents_bytes / 1024) || " KB"
                else (subsequents_bytes / 1024 / 1024) || " MB" end)
          || " (" || (((1 + subsequents_bytes) * 100)
                       / (1+accesses_bytes)) || "%)"
     from (select *, count (*) accesses,
       sum (size) accesses_bytes,
       sum (uid not in (select uid from l where ROWID < louter.ROWID)) firsts,
       sum (case when uid not in (select uid from l where ROWID < louter.ROWID)
                   then size
                 else 0
            end) firsts_bytes,
       sum (uid in (select uid from l where ROWID < louter.ROWID)) subsequents,
       sum (case when uid in (select uid from l where ROWID < louter.ROWID)
                   then size
                 else 0
            end) subsequents_bytes
       from l as louter
       group by year, doy
       order by time);
   
   select filename || " (" || size || "): "
     || previous2_delta || ", " || previous_delta
     || " +++ " || next_delta || ", " || next2_delta || " => " || scan
     from l;

   select "Accesses in top-level directories: ";
   select dirs || " accesses in " || dir
     from (select case
        when substr (filename,'$hl'+1,1)="/" then substr (filename, '$hl', 1)
        when substr (filename,'$hl'+2,1)="/" then substr (filename, '$hl', 2)
        when substr (filename,'$hl'+3,1)="/" then substr (filename, '$hl', 3)
        when substr (filename,'$hl'+4,1)="/" then substr (filename, '$hl', 4)
        when substr (filename,'$hl'+5,1)="/" then substr (filename, '$hl', 5)
        when substr (filename,'$hl'+6,1)="/" then substr (filename, '$hl', 6)
        when substr (filename,'$hl'+7,1)="/" then substr (filename, '$hl', 7)
        when substr (filename,'$hl'+8,1)="/" then substr (filename, '$hl', 8)
        when substr (filename,'$hl'+9,1)="/" then substr (filename, '$hl', 9)
        when substr (filename,'$hl'+10,1)="/" then substr (filename, '$hl', 10)
        when substr (filename,'$hl'+11,1)="/" then substr (filename, '$hl', 11)
        when substr (filename,'$hl'+12,1)="/" then substr (filename, '$hl', 12)
        when substr (filename,'$hl'+13,1)="/" then substr (filename, '$hl', 13)
        when substr (filename,'$hl'+14,1)="/" then substr (filename, '$hl', 14)
        when substr (filename,'$hl'+15,1)="/" then substr (filename, '$hl', 15)
        when substr (filename,'$hl'+16,1)="/" then substr (filename, '$hl', 16)
        when substr (filename,'$hl'+17,1)="/" then substr (filename, '$hl', 17)
        when substr (filename,'$hl'+18,1)="/" then substr (filename, '$hl', 18)
        when substr (filename,'$hl'+19,1)="/" then substr (filename, '$hl', 19)
        when substr (filename,'$hl'+20,1)="/" then substr (filename, '$hl', 20)
        else filename
       end dir,
       count (*) dirs
      from log_scans
      group by dir)
     group by dirs order by dirs desc;
  '
